// OPD2413 cascade NT36536, for the pinned XN242 firmware only.
static constexpr unsigned erhaiNtFirmwareSize = 258048;
static constexpr uint8_t erhaiNtTrim[] = {0x15, 0x0e, 0x00, 0x23, 0x65, 0x03};
#if ERHAI_VERIFY_STORAGE
static bool erhai_storage_hash(const void *, size_t, uint8_t *);
#else
extern "C" void erhai_local_sha256(const void *, size_t, uint8_t *);
#endif

static bool erhai_nt_verify_firmware(const uint8_t *image)
{
    constexpr uint8_t expected[] = {
        0x3e, 0x35, 0x96, 0x89, 0x59, 0x75, 0xb2, 0x84, 0x59, 0xb1, 0x93, 0x7e, 0xa3, 0x5c, 0x81, 0x9b,
        0x96, 0x38, 0x6b, 0x82, 0xcb, 0xdc, 0xc9, 0xc6, 0x15, 0x92, 0xaf, 0x84, 0x88, 0xdb, 0x70, 0x2b,
    };
    uint8_t digest[sizeof(expected)];
#if ERHAI_VERIFY_STORAGE
    if (!erhai_storage_hash(image, erhaiNtFirmwareSize, digest)) return false;
#else
    erhai_local_sha256(image, erhaiNtFirmwareSize, digest);
#endif
    return memcmp(digest, expected, sizeof(expected)) == 0;
}

static int erhai_nt_capture_reports(ErhaiTouchGpi &gpi, unsigned divider, socket_t connection,
                                    uint32_t &frames, bool sendHid
#if ERHAI_TOUCH_IRQ_GATE
                                    , IOMemoryMap *touchIrq
#endif
                                    )
{
    const auto sendTrace = [&](const void *data, uint32_t size) {
        return !connection || erhai_send_all(connection, data, size);
    };
    // The firmware info and touch report both live on the event-buffer page.
    const uint8_t page[] = {0xff, 0x26, 0x12};
    int result = gpi.transfer(page, nullptr, sizeof(page), divider);
    if (result) return result;
    const uint8_t infoCommand[40] = {0x78};
    uint8_t infoReply[sizeof(infoCommand)] = {};
    result = gpi.transfer(infoCommand, infoReply, sizeof(infoReply), divider);
    if (result) return result;
    if (infoReply[2] != 0x1d || unsigned(infoReply[2]) + infoReply[3] != 0xff) return -19;
    const uint32_t format[] = {0xf6, infoReply[14] == 0xf1 ? 16U : 12U};
    if (!sendTrace(format, sizeof(format))) return -5;

    // The event-buffer page stays selected throughout this capture.
    const auto reportControllerState = [&]() -> int {
        const uint8_t commandBytes[4] = {0x50};
        uint8_t hostReply[sizeof(commandBytes)] = {};
        int status = gpi.transfer(commandBytes, hostReply, sizeof(hostReply), divider);
        if (status) return status;
        const uint8_t resetCommand[8] = {0x60};
        uint8_t resetReply[sizeof(resetCommand)] = {};
        status = gpi.transfer(resetCommand, resetReply, sizeof(resetReply), divider);
        if (status) return status;
        const uint32_t state[] = {
            0x106, frames,
            0x107, uint32_t(hostReply[2]) | uint32_t(hostReply[3]) << 8,
            0x108, uint32_t(resetReply[2]) | uint32_t(resetReply[3]) << 8 |
                   uint32_t(resetReply[4]) << 16 | uint32_t(resetReply[5]) << 24,
            0x109, uint32_t(resetReply[6]) | uint32_t(resetReply[7]) << 8,
        };
        return sendTrace(state, sizeof(state)) ? 0 : -5;
    };
    result = reportControllerState();
    if (result) return result;

    const uint8_t command[ErhaiTouchGpi::maxBufferedTransfer] = {};
    uint8_t reply[sizeof(command)] = {};
    const uint32_t header[] = {0xf7, sizeof(reply)};
    const unsigned maxFrames = sendHid ? 2400 : 900;
#if ERHAI_HID_EARLY
    bool checkedBadPage = false;
#endif
#if ERHAI_HOME_INPUT
    unsigned invalidFrames = 0;
    unsigned previousMask = 0;
    unsigned checksumFailures = 0;
    unsigned recoveryFrames = 0;
    unsigned fdFrames = 0;
#endif
#if ERHAI_TOUCH_IRQ_GATE
#if !ERHAI_TOUCH_IRQ_LATCH
    bool sawHigh = false;
#endif
    unsigned polls = 0;
    unsigned highSamples = 0;
    const uint32_t irqBefore[] = {
        0x123, erhai_touch_reg(touchIrq, 0x0),
        0x124, erhai_touch_reg(touchIrq, 0x8),
        0x125, erhai_touch_reg(touchIrq, 0xc),
    };
    if (!sendTrace(irqBefore, sizeof(irqBefore))) return -5;
#endif
    while (!connection || frames < maxFrames) {
#if ERHAI_TOUCH_IRQ_GATE
#if ERHAI_TOUCH_IRQ_LATCH
        if (connection && polls == 45000) break;
#else
        // Sample the input level before reading any report. A sustained low
        // counts once, and an initially low line must first return high.
        if (polls == 22500) break;
#endif
        const bool high = (erhai_touch_reg(touchIrq, 4) & 1) != 0;
        highSamples += high;
        if (polls++ % 500 == 0 && connection) {
            const uint32_t heartbeat[] = {0x120, polls - 1, 0x121, frames, 0x122, high};
            if (!sendTrace(heartbeat, sizeof(heartbeat))) return -5;
        }
#if ERHAI_TOUCH_IRQ_LATCH
        if (!(erhai_touch_reg(touchIrq, 12) & 1)) {
            IOSleep(2);
            continue;
        }
        // Acknowledge first so a later edge can latch during the SPI read.
        erhai_touch_write(touchIrq, 12, 0);
#else
        if (high || !sawHigh) {
            sawHigh = high;
            IOSleep(2);
            continue;
        }
        sawHigh = false;
#endif
#endif
        result = gpi.transfer(command, reply, sizeof(reply), divider);
        if (result) return result;
#if ERHAI_HID_EARLY
        constexpr uint8_t repeatedFd[] = {0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd};
        if (!checkedBadPage && !memcmp(reply + 2, repeatedFd, sizeof(repeatedFd))) {
            checkedBadPage = true;
            const uint32_t firstBad[] = {0x11d, frames + 1};
            if (!sendTrace(firstBad, sizeof(firstBad))) return -5;
            result = reportControllerState();
            if (result) return result;
            result = gpi.transfer(page, nullptr, sizeof(page), divider);
            if (result) return result;
            uint8_t afterPage[sizeof(reply)] = {};
            result = gpi.transfer(command, afterPage, sizeof(afterPage), divider);
            if (result) return result;
            uint8_t sum = 0;
            for (unsigned i = 2; i <= 66; ++i) sum += afterPage[i];
            const uint32_t pageCheck[] = {
                0x11e, sum,
                0x11f, !memcmp(afterPage + 2, repeatedFd, sizeof(repeatedFd)),
            };
            if (!sendTrace(pageCheck, sizeof(pageCheck))) return -5;
        }
#endif
        if (!sendHid) {
            if (!sendTrace(header, sizeof(header)) ||
                !sendTrace(reply, sizeof(reply))) return -5;
        }
#if ERHAI_HOME_INPUT
        else {
            if (frames % 250 == 0) {
                const uint32_t progress[] = {0xf9, frames};
                if (!sendTrace(progress, sizeof(progress))) return -5;
            }
            uint8_t checksum = 0;
            const uint8_t recovery[] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
#if !ERHAI_HID_EARLY
            constexpr uint8_t repeatedFd[] = {0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd};
#endif
            for (unsigned i = 2; i <= 66; ++i) checksum += reply[i];
            const bool recoveryFrame = !memcmp(reply + 2, recovery, sizeof(recovery));
            checksumFailures += checksum != 0;
            recoveryFrames += recoveryFrame;
            fdFrames += !memcmp(reply + 2, repeatedFd, sizeof(repeatedFd));
            if (checksum || recoveryFrame) {
                // A short corrupted burst must not strand the last touch down.
                if (++invalidFrames == 3) {
                    erhai_publish_contacts(nullptr, nullptr, nullptr, 0);
                    previousMask = 0;
                }
#if ERHAI_TOUCH_AUTO
                if (!connection && invalidFrames >= 32) return -5;
#endif
            } else {
                invalidFrames = 0;
                int xs[10], ys[10], ids[10];
                unsigned count = 0, seen = 0;
                const bool wide = format[1] == 16;
                const unsigned maxX = wide ? 24000 : 2400;
                const unsigned maxY = wide ? 33920 : 3392;
                for (unsigned slot = 0; slot < 10; ++slot) {
                    const auto *finger = reply + 2 + slot * 6;
                    const unsigned id = finger[0] >> 3, state = finger[0] & 7;
                    if (!id || id > 10 || (state != 1 && state != 2) || (seen & (1u << id))) continue;
                    const unsigned x = wide ? (unsigned(finger[1]) << 8) | finger[2] :
                        (unsigned(finger[1]) << 4) | (finger[3] >> 4);
                    const unsigned y = wide ? (unsigned(finger[3]) << 8) | finger[4] :
                        (unsigned(finger[2]) << 4) | (finger[3] & 15);
                    if (x >= maxX || y >= maxY) continue;
                    seen |= 1u << id;
                    // Board DT inverts the controller X axis, then swaps X and Y.
                    // The reused HID producer takes 30480 by 20320 source units.
                    xs[count] = y * 30479 / (maxY - 1);
                    ys[count] = (maxX - 1 - x) * 20319 / (maxX - 1);
                    ids[count++] = id;
                }
                erhai_publish_contacts(xs, ys, ids, count);
                if (connection && (count || previousMask)) {
                    const uint32_t sample[] = {0x100, frames + 1,
                        0x101, count ? uint32_t(xs[0]) : UINT32_MAX,
                        0x102, count ? uint32_t(ys[0]) : UINT32_MAX};
                    if (!sendTrace(sample, sizeof(sample)) ||
                        !sendTrace(header, sizeof(header)) ||
                        !sendTrace(reply, sizeof(reply))) return -5;
                }
                previousMask = seen;
            }
        }
#endif
        ++frames;
#if !ERHAI_TOUCH_IRQ_GATE
        IOSleep(20);
#endif
    }
#if ERHAI_TOUCH_IRQ_GATE
    const uint32_t irqAfter[] = {
        0x126, polls,
        0x127, highSamples,
        0x128, erhai_touch_reg(touchIrq, 0xc),
    };
    if (!sendTrace(irqAfter, sizeof(irqAfter))) return -5;
#endif
#if ERHAI_HOME_INPUT
    if (sendHid) {
        const uint32_t counters[] = {0x10a, checksumFailures, 0x10b, recoveryFrames, 0x10c, fdFrames};
        if (!sendTrace(counters, sizeof(counters))) return -5;
    }
#endif
    return reportControllerState();
}

template <class Trace>
static int erhai_nt_load_sram(ErhaiTouchGpi &gpi, unsigned divider, const uint8_t *image,
                              Trace trace, uint32_t &written, uint8_t (&firmwareInfo)[8])
{
    constexpr uint32_t eventBuffer = 0x130900;
    constexpr uint32_t autoCopy = 0x1fc825;
    // Sized from the pinned image's manifest, including each final byte.
    struct Partition { uint32_t sram, offset, bytes, crc; };
    constexpr Partition partitions[] = {
        {0x000000, 0x00250, 0x1fe44, 0xa6b863ae},
        {0x100000, 0x22000, 0x159f8, 0xdf14ad7e},
        {0x1159f8, 0x00000, 0x00100, 0x55855333},
        {0x115af8, 0x00100, 0x00100, 0xcb35a8ee},
    };
    static_assert(partitions[1].offset + partitions[1].bytes <= erhaiNtFirmwareSize);
    uint8_t packet[ErhaiTouchGpi::maxTransmit];
    const auto selectPage = [&](uint32_t address) {
        const uint8_t select[] = {0xff, uint8_t(address >> 15), uint8_t(address >> 7)};
        return gpi.transfer(select, nullptr, sizeof(select), divider);
    };
    const auto writeBytes = [&](uint32_t address, const uint8_t *bytes, unsigned count) {
        const int result = selectPage(address);
        if (result) return result;
        packet[0] = uint8_t(address & 0x7f) | 0x80;
        memcpy(packet + 1, bytes, count);
        return gpi.transfer(packet, nullptr, count + 1, divider);
    };
    const auto writeRegister = [&](uint32_t address, uint32_t value, unsigned count) {
        const uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16),
                                  uint8_t(value >> 24)};
        return writeBytes(address, bytes, count);
    };
    const auto readBytes = [&](uint32_t address, uint8_t *bytes, unsigned count) {
        const int result = selectPage(address);
        if (result) return result;
        uint8_t command[40] = {uint8_t(address & 0x7f)};
        uint8_t reply[sizeof(command)] = {};
        const int transferred = gpi.transfer(command, reply, count + 2, divider);
        if (!transferred) memcpy(bytes, reply + 2, count);
        return transferred;
    };
    uint8_t trim[sizeof(erhaiNtTrim)];
    int result = readBytes(0x1fb104, trim, sizeof(trim));
    if (result) return result;
    if (memcmp(trim, erhaiNtTrim, sizeof(trim))) return -19;

    trace(0xf0, 1, false);
    // Controller MCU bootloader reset only. The panel shares this package.
    result = writeRegister(0x1fb43e, 0x69, 1);
    if (result) return result;
    IOSleep(5);
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t crcBase = 0x1fcc30 + 16 * i;
        const auto &partition = partitions[i];
        result = writeRegister(crcBase, partition.sram, 3);
        if (!result) result = writeRegister(crcBase + 4, partition.bytes - 1, 3);
        if (!result) result = writeRegister(crcBase + 8, partition.crc, 4);
        if (result) return result;
    }
    result = writeRegister(autoCopy, 0x56, 1);
    if (result) return result;
    for (const auto &partition : partitions) {
        trace(0xf1, partition.sram, false);
        for (unsigned offset = 0; offset < partition.bytes; offset += 1024) {
            const unsigned remaining = partition.bytes - offset;
            const unsigned count = remaining < 1024 ? remaining : 1024;
            result = writeBytes(partition.sram + offset, image + partition.offset + offset, count);
            if (result) return result;
            written += count;
        }
        trace(0xf2, written, false);
    }
    uint8_t copyState = 0xff;
    for (unsigned i = 0; i < 200; ++i) {
        result = readBytes(autoCopy, &copyState, 1);
        if (result || !copyState) break;
        IOSleep(1);
    }
    trace(0xf3, copyState, false);
    if (result || copyState) return result ? result : -110;

    result = writeRegister(eventBuffer + 0x60, 0, 1);
    if (!result) result = writeRegister(eventBuffer + 0x50, 0xae, 1);
    if (!result) result = writeRegister(0x1fb50d, 1, 1);
    if (result) return result;
    IOSleep(5);
    uint8_t resetState = 0;
    for (unsigned i = 0; i < 11; ++i) {
        IOSleep(10);
        result = readBytes(eventBuffer + 0x60, &resetState, 1);
        if (result || (resetState >= 0xa0 && resetState <= 0xaf)) break;
    }
    trace(0xf4, resetState, false);
    if (result || resetState < 0xa0 || resetState > 0xaf) return result ? result : -110;
    // Full firmware info command, including the command and dummy bytes.
    uint8_t info[38];
    result = readBytes(eventBuffer + 0x78, info, sizeof(info));
    if (result) return result;
    memcpy(firmwareInfo, info, sizeof(firmwareInfo));
    trace(0xf5, info[0], false);
    if (info[0] != 0x1d || unsigned(info[0]) + info[1] != 0xff ||
        info[2] != 60 || info[3] != 40) return -5;

    // Match the OPD2413 Linux panel-follower resume sequence: initialize
    // the pen mailbox after the firmware download, before reading reports.
    IOSleep(35);
    const uint8_t penMailbox[] = {0x7b, 0x01};
    result = writeBytes(eventBuffer + 0x50, penMailbox, sizeof(penMailbox));
    trace(0x10d, uint32_t(result), false);
    if (result) return result;
    uint8_t mailboxState[2] = {};
    for (unsigned i = 0; i < 10; ++i) {
        IOSleep(10);
        result = readBytes(eventBuffer + 0x50, mailboxState, sizeof(mailboxState));
        if (result || !mailboxState[0]) break;
    }
    trace(0x10e, uint32_t(mailboxState[0]) | uint32_t(mailboxState[1]) << 8, false);
    if (result) return result;
    return 0;
}
