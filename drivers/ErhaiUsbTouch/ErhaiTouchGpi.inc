// OPD2413 GPI DMA, using the register and descriptor layout of qcom,sm6350-gpi-dma.
// The board DT enables GPII 1..4 for the wrap1 engine and assigns stream 0xb6.
static constexpr unsigned erhaiGpiStride = 0x4000;
static constexpr unsigned erhaiGpiTx = 0;
static constexpr unsigned erhaiGpiRx = 0x80;
static constexpr unsigned erhaiGpiEvent = 0x1000;
static constexpr unsigned erhaiGpiTxDoorbell = 0x2000;
static constexpr unsigned erhaiGpiRxDoorbell = 0x2008;
static constexpr unsigned erhaiGpiEventDoorbell = 0x2100;

static unsigned erhai_gpi_state(uint32_t control)
{
    return (control >> 20) & 15;
}

[[maybe_unused]] static void erhai_touch_gpi_info(socket_t connection)
{
    uint32_t report[21] = {};
    IOMemoryMap *gpi = erhai_touch_map(0xa10000, false, 5 * erhaiGpiStride);
    if (gpi) {
        report[0] = 1;
        for (unsigned id = 1; id <= 4; ++id) {
            const unsigned offset = id * erhaiGpiStride;
            auto *entry = report + 1 + 5 * (id - 1);
            entry[0] = erhai_touch_reg(gpi, offset + erhaiGpiTx);
            entry[1] = erhai_touch_reg(gpi, offset + erhaiGpiRx);
            entry[2] = erhai_touch_reg(gpi, offset + erhaiGpiEvent);
            entry[3] = erhai_touch_reg(gpi, offset + 0x3088);
            entry[4] = erhai_touch_reg(gpi, offset + 0x3200);
        }
        gpi->release();
    }
    erhai_send_all(connection, report, sizeof(report));
}

struct ErhaiGpiDescriptor {
    uint32_t words[4];
};
static_assert(sizeof(ErhaiGpiDescriptor) == 16);

struct ErhaiTouchGpi {
    IOMemoryMap *registers = nullptr;
    unsigned base = 0;
    uint8_t *memory = nullptr;
    uint64_t physical = 0;
    unsigned txHead = 0, rxHead = 0, eventHead = 0;
    bool issuedCommand = false, commandTimedOut = false;
    bool loopback = false;
    uint32_t savedIrqMask = 0;
    uint32_t lastEvent[4] = {};
    static constexpr unsigned page = 4096;
    static constexpr unsigned eventBytes = page, channelBytes = 1024;
    static constexpr unsigned maxBufferedTransfer = 122;
    // Novatek firmware writes use a one-byte SRAM offset and up to 1 KiB.
    static constexpr unsigned maxTransmit = 1025;
    static_assert(channelBytes + maxTransmit <= page);

    uint32_t read(unsigned offset) const { return erhai_touch_reg(registers, base + offset); }
    void write(unsigned offset, uint32_t value) const
    {
        erhai_touch_write(registers, base + offset, value);
    }

    unsigned findIdleInstance(unsigned first)
    {
        const unsigned contexts[] = {erhaiGpiTx, erhaiGpiRx, erhaiGpiEvent};
        for (unsigned id = first; id <= 4; ++id) {
            base = id * erhaiGpiStride;
            bool idle = true;
            for (unsigned context : contexts) {
                const auto control = read(context);
                idle &= control != UINT32_MAX && !erhai_gpi_state(control) &&
                    !read(context + 4) && !read(context + 8) && !read(context + 12);
            }
            if (idle) return id;
        }
        return 0;
    }

    bool command(unsigned context, unsigned opcode, unsigned expected)
    {
        const bool event = context == erhaiGpiEvent;
        issuedCommand = true;
        write(event ? 0x3010 : 0x3008, (opcode << 24) | unsigned(context == erhaiGpiRx));
        for (unsigned poll = 0; poll < 2000; ++poll) {
            if (erhai_gpi_state(read(context)) == expected) {
                write(event ? 0x30a4 : 0x30a0, 1U << unsigned(context == erhaiGpiRx));
                return true;
            }
            IODelay(25);
        }
        commandTimedOut = true;
        return false;
    }

    void ring(unsigned context, unsigned doorbell, uint64_t address, unsigned length,
              uint32_t config)
    {
        write(context, config);
        write(context + 4, length);
        write(context + 8, uint32_t(address));
        write(context + 12, uint32_t(address >> 32));
        write(doorbell + 4, uint32_t(address >> 32));
    }

    bool start()
    {
        savedIrqMask = read(0x3088);
        write(0x3088, savedIrqMask & ~0x7fU);
        if (!command(erhaiGpiEvent, 0, 1)) return false;
        ring(erhaiGpiEvent, erhaiGpiEventDoorbell, physical, eventBytes, 0x10010002);
        const unsigned eventOffsets[] = {0x20, 0x28, 0x2c, 0x30, 0x34};
        for (unsigned offset : eventOffsets)
            write(erhaiGpiEvent + offset, 0);
        write(erhaiGpiEventDoorbell, uint32_t(physical + eventBytes - 16));
        for (unsigned channel = 0; channel < 2; ++channel) {
            const unsigned context = channel ? erhaiGpiRx : erhaiGpiTx;
            if (!command(context, 0, 1)) return false;
            ring(context, channel ? erhaiGpiRxDoorbell : erhaiGpiTxDoorbell,
                 physical + (channel + 1) * page, channelBytes, channel ? 0x10000002 : 0x1000000a);
            write(context + 0x5c, 1);
            write(context + 0x60, ((channel ^ 1) << 16) | 0x14); // SPI protocol 1, SE4.
            write(context + 0x64, 0);
            write(context + 0x68, 0);
            write(context + 0x6c, 0);
        }
        return command(erhaiGpiTx, 1, 2) && command(erhaiGpiRx, 1, 2);
    }

    template <class Trace>
    bool stop(Trace trace)
    {
        // SPI channels are paired. Stop both before resetting either ring.
        const unsigned contexts[] = {erhaiGpiTx, erhaiGpiRx};
        for (unsigned context : contexts) {
            const unsigned state = erhai_gpi_state(read(context));
            trace(0xc0 | (context >> 7), state);
            if (state == 2 && !command(context, 2, 3)) return false;
        }
        for (unsigned context : contexts) {
            const unsigned state = erhai_gpi_state(read(context));
            trace(0xc2 | (context >> 7), state);
            if ((state == 3 || state == 15) && !command(context, 9, 1)) return false;
            if (state != 0 && state != 1 && state != 3 && state != 15) return false;
        }
        for (unsigned context : contexts) {
            const unsigned state = erhai_gpi_state(read(context));
            trace(0xc4 | (context >> 7), state);
            if (state && (state != 1 || !command(context, 10, 0))) return false;
        }
        const unsigned state = erhai_gpi_state(read(erhaiGpiEvent));
        trace(0xc6, state);
        if (state && (state != 1 || !command(erhaiGpiEvent, 10, 0))) return false;
        write(0x30c0, 1);
        write(0x3088, savedIrqMask);
        trace(0xc7, savedIrqMask);
        return true;
    }

    void submitReceive(unsigned length)
    {
        auto *rx = reinterpret_cast<ErhaiGpiDescriptor *>(memory + 2 * page + rxHead);
        const uint64_t target = physical + 3 * page;
        *rx = {{uint32_t(target), uint32_t(target >> 32), length, 0x00100000}};
        memset(memory + 3 * page, 0x5a, length);
        erhai_touch_cache<ErhaiTouchCache::CleanInvalidate>(nullptr, memory + 3 * page, length);
        erhai_touch_cache<ErhaiTouchCache::Clean>(nullptr, rx, 16);
        rxHead = (rxHead + sizeof(*rx)) % channelBytes;
        write(erhaiGpiRxDoorbell, uint32_t(physical + 2 * page + rxHead));
    }

    int transfer(const uint8_t *send, uint8_t *receive, unsigned length, unsigned divider,
                 socket_t traceConnection = nullptr, bool receiveOnly = false,
                 IOMemoryMap *engine = nullptr, IOMemoryMap *smmu = nullptr,
                 unsigned bankBase = 0, bool configure = true)
    {
        const unsigned descriptorBytes = (configure ? 3 : 2) * sizeof(ErhaiGpiDescriptor);
        if (!send || !length || length > (receive ? maxBufferedTransfer : maxTransmit) ||
            !divider || divider > 0xfff) return -22;
        ErhaiGpiDescriptor descriptors[3];
        auto *tx = descriptors;
        // 8 bits/word, mode 0, packed TX and RX. The clock source is XO.
        if (configure)
            *tx++ = {{0x03000004 | (loopback ? (1U << 8) : 0u), 0, divider, 0x00220001}};
        // Duplex clocks are determined by TX; the RX descriptor gives buffer size.
        if (receiveOnly)
            tx[0] = {{2, 0, length, 0x00200900}}; // SPI_RX, IEOB and LINK.
        else
            tx[0] = {{receive ? 3u : 1u, 0, 0, receive ? 0x00200801u : 0x00200001u}};
        if (receive || length > 8) {
            // Keep the transmit payload outside the transfer ring and RX page.
            const size_t payloadOffset = page + channelBytes;
            auto *payload = memory + payloadOffset;
            const uint64_t payloadAddress = physical + payloadOffset;
            memcpy(payload, send, length);
            erhai_touch_cache<ErhaiTouchCache::Clean>(nullptr, payload, length);
            tx[1] = {{uint32_t(payloadAddress), uint32_t(payloadAddress >> 32), length, 0x00100200}};
        } else {
            tx[1] = {{0, 0, length, 0x00110200}};
            memcpy(tx[1].words, send, length);
        }
        const unsigned finalOffset = (txHead + descriptorBytes - sizeof(ErhaiGpiDescriptor)) % channelBytes;
        const uint64_t final = physical + page + finalOffset;
        for (unsigned i = 0; i < descriptorBytes / sizeof(ErhaiGpiDescriptor); ++i) {
            auto *entry = reinterpret_cast<ErhaiGpiDescriptor *>(memory + page + txHead);
            *entry = descriptors[i];
            erhai_touch_cache<ErhaiTouchCache::Clean>(nullptr, entry, sizeof(*entry));
            txHead = (txHead + sizeof(*entry)) % channelBytes;
        }

        auto trace = [&](uint32_t point, uint32_t value) {
            if (!traceConnection) return;
            const uint32_t record[] = {point, value};
            erhai_send_all(traceConnection, record, sizeof(record));
        };
        trace(0xbf, tx[1].words[3]);
        if (receive) {
            trace(0xb6, length);
            submitReceive(length);
            trace(0xbe, rxHead);
            if (engine) {
                trace(0xcf, read(erhaiGpiRx + 0x10));
                IOSleep(16);
            }
        }
        trace(0xb7, txHead);
        const bool screenTrace = receive && smmu && boot_progress_root;
        if (screenTrace) {
            boot_progress_root("TOUCH 1 BEFORE DMA START", nullptr, length);
            IOSleep(500);
        }
        write(erhaiGpiTxDoorbell, uint32_t(physical + page + txHead));
        if (screenTrace)
            boot_progress_root("TOUCH 2 DMA START RETURNED", nullptr, 0);
        trace(0xb8, eventHead);
        if (engine && traceConnection) {
            const uint32_t snapshot[] = {
                0xd0, erhai_touch_reg(engine, SE_GENI_STATUS),
                0xd1, erhai_touch_reg(engine, SE_GENI_M_IRQ_STATUS),
                0xd2, erhai_touch_reg(engine, SE_DMA_RX_IRQ_STAT),
                0xd3, read(erhaiGpiRx + 0x10),
                0xd4, read(erhaiGpiTx + 0x10),
            };
            erhai_send_all(traceConnection, snapshot, sizeof(snapshot));
        }

        uint32_t previousProgress[4] = {};
        for (unsigned poll = 0; poll < 2000; ++poll) {
            if (receive && traceConnection && smmu) {
                const uint32_t progress[] = {
                    read(erhaiGpiTx + 0x10),
                    read(erhaiGpiRx + 0x10),
                    erhai_touch_reg(smmu, bankBase + 0x58),
                    erhai_touch_reg(smmu, 0x48),
                };
                if (!poll || memcmp(progress, previousProgress, sizeof(progress))) {
                    if (screenTrace && !poll)
                        boot_progress_root("TOUCH 3 DMA STATUS SAMPLED", nullptr, progress[2]);
                    if (screenTrace && (progress[2] & contextFaultBits))
                        boot_progress_root("TOUCH SMMU FAULT STATUS", nullptr, progress[2]);
                    memcpy(previousProgress, progress, sizeof(progress));
                    const uint32_t record[] = {
                        0xe0, progress[0], 0xe1, progress[1],
                        0xe2, progress[2], 0xe3, progress[3],
                    };
                    erhai_send_all(traceConnection, record, sizeof(record));
                }
            }
            const uint32_t pointer = read(erhaiGpiEvent + 0x10);
            if (!pointer && uint32_t(physical)) {
                IODelay(25);
                continue;
            }
            const uint32_t head = pointer - uint32_t(physical);
            if (head >= eventBytes || (head & 15)) return -5;
            while (eventHead != head) {
                auto *event = reinterpret_cast<ErhaiGpiDescriptor *>(memory + eventHead);
                erhai_touch_cache<ErhaiTouchCache::Invalidate>(nullptr, event, 16);
                memcpy(lastEvent, event, sizeof(lastEvent));
                write(erhaiGpiEventDoorbell, uint32_t(physical + eventHead));
                eventHead = (eventHead + 16) & (eventBytes - 1);
                const uint64_t address = lastEvent[0] | (uint64_t(lastEvent[1]) << 32);
                if (receiveOnly) {
                    trace(0xcc, lastEvent[0]);
                    trace(0xcd, lastEvent[2]);
                    trace(0xce, lastEvent[3]);
                }
                const unsigned code = lastEvent[2] >> 24;
                const unsigned rxOffset = (rxHead + channelBytes - sizeof(ErhaiGpiDescriptor)) % channelBytes;
                const uint64_t rxDescriptor = physical + 2 * page + rxOffset;
                const unsigned eventType = (lastEvent[3] >> 16) & 0xff;
                const unsigned channel = lastEvent[3] >> 24;
                if (eventType != 0x22) continue;
                if (receiveOnly) {
                    if (channel != 1 || address != rxDescriptor) continue;
                    if (code == 4) continue; // End of block alone is not completion.
                    if (code != 1 && code != 2) return -5;
                } else {
                    if (channel != 0 || address != final) continue;
                    if (code != 1 && code != 2) return -5;
                }
                if ((lastEvent[2] & 0x00ffffff) != length) return -5;
                if (receive) {
                    erhai_touch_cache<ErhaiTouchCache::Invalidate>(nullptr, memory + 3 * page, length);
                    memcpy(receive, memory + 3 * page, length);
                }
                return 0;
            }
            IODelay(25);
        }
        return -110;
    }
};
