// OPD2413 NT36536: one bounded read of the event buffer reset state.
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" {
#include <ArmSmmu4K.h>
#include <GeniSpi.h>
}
#include <SmmuTakeover.h>

static void erhai_touch_write(void *context, uint32_t offset, uint32_t value)
{
    auto *mapping = static_cast<IOMemoryMap *>(context);
    *reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress() + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t erhai_touch_read(void *context, uint32_t offset)
{
    return erhai_touch_reg(static_cast<IOMemoryMap *>(context), offset);
}

static void erhai_touch_delay(void *, unsigned us) { IODelay(us); }

static bool erhai_touch_wait_idle(IOMemoryMap *engine, uint32_t &status)
{
    for (unsigned i = 0; i < 1000; ++i) {
        status = erhai_touch_reg(engine, SE_GENI_STATUS);
        if (!(status & GENI_STATUS_M_ACTIVE)) return true;
        IODelay(10);
    }
    return false;
}

// Both DMA buffers have a page to themselves. Invalidating RX cannot
// discard a neighbouring kernel object's dirty cache line.
enum class ErhaiTouchCache { Clean, CleanInvalidate, Invalidate };
template <ErhaiTouchCache operation>
static void erhai_touch_cache(void *, void *bytes, size_t size)
{
    uintptr_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = 4ULL << ((ctr >> 16) & 15);
    const auto start = reinterpret_cast<uintptr_t>(bytes);
    for (uintptr_t at = start & ~(line - 1); at < start + size; at += line) {
        if (operation == ErhaiTouchCache::Clean) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
        if (operation == ErhaiTouchCache::CleanInvalidate) __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
        if (operation == ErhaiTouchCache::Invalidate) __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static constexpr uint32_t contextFaultBits = 0xc00001feU; // Exclude FSR.FORMAT.
#include "ErhaiTouchGpi.inc"
#include "ErhaiTouchFirmware.inc"

[[maybe_unused]] static void erhai_touch_smmu_info(socket_t connection)
{
    enum { HeaderWords = 6, BankWords = 10, BankCount = 8 };
    uint32_t report[HeaderWords + BankWords * BankCount] = {};
    auto *smmu = erhai_touch_map(UFS_SMMU_BASE, false, UFS_SMMU_SIZE);
    if (smmu) {
        auto read = [smmu](unsigned offset) { return erhai_touch_reg(smmu, offset); };
        report[0] = 1;
        report[1] = read(0x20);
        report[2] = read(0x24);
        report[3] = read(0x28);
        const auto plan = planUfsSmmu(0xb6, 0x40000000, 16 * 1024, read);
        report[4] = plan.error ? UINT32_MAX : plan.bank;
        const unsigned banks = report[2] & 255;
        const unsigned pages = 1U << (((report[2] >> 28) & 7) + 1);
        if (!(report[2] >> 31) && banks && banks <= 128 && pages <= 128 &&
            (pages + banks) * 4096 <= UFS_SMMU_SIZE) {
            report[5] = banks < BankCount ? banks : unsigned(BankCount);
            for (unsigned i = 0; i < report[5]; ++i) {
                const unsigned bank = banks - 1 - i;
                const unsigned bankBase = (pages + bank) * 4096;
                const unsigned offsets[] = {0, 0x10, 0x20, 0x24, 0x30, 0x38, 0x58};
                auto *entry = report + HeaderWords + i * BankWords;
                entry[0] = bank;
                entry[1] = read(0x1000 + 4 * bank);
                entry[2] = read(0x1800 + 4 * bank);
                for (unsigned reg = 0; reg < sizeof(offsets) / sizeof(offsets[0]); ++reg)
                    entry[3 + reg] = read(bankBase + offsets[reg]);
            }
        }
        smmu->release();
    }
    erhai_send_all(connection, report, sizeof(report));
}

[[maybe_unused]] static void erhai_touch_smmu_routes(socket_t connection)
{
    constexpr uint16_t streams[] = {0xa3, 0xb6};
    constexpr unsigned headerWords = 4, routeWords = 9;
    uint32_t report[headerWords + 2 * routeWords] = {};
    auto *smmu = erhai_touch_map(UFS_SMMU_BASE, false, UFS_SMMU_SIZE);
    if (smmu) {
        auto read = [smmu](unsigned offset) { return erhai_touch_reg(smmu, offset); };
        report[0] = 1;
        const uint32_t control = report[1] = read(0);
        const uint32_t idr0 = report[2] = read(0x20);
        const uint32_t idr1 = report[3] = read(0x24);
        const unsigned banks = idr1 & 255;
        const unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
        const unsigned groups = idr0 & 255;
        const unsigned slots = groups < 128 ? groups : 128;
        if (!(idr1 >> 31) && banks && banks <= 128 && slots &&
            (pages + banks) * 4096 <= UFS_SMMU_SIZE) {
            report[0] = 2;
            const bool extended = (idr0 & (1u << 8)) && (control & (1u << 3));
            for (unsigned slot = 0; slot < slots; ++slot) {
                const uint32_t smr = read(0x800 + 4 * slot);
                const uint32_t s2cr = read(0xc00 + 4 * slot);
                if (!(extended ? (s2cr & (1u << 10)) : (smr >> 31))) continue;
                const uint32_t mask = (smr >> 16) & (extended ? 0xffffU : 0x7fffU);
                for (unsigned i = 0; i < 2; ++i) {
                    if ((smr ^ streams[i]) & 0xffffU & ~mask) continue;
                    auto *route = report + headerWords + i * routeWords;
                    if (route[0]++) continue;
                    route[1] = slot;
                    route[2] = smr;
                    route[3] = s2cr;
                    const unsigned bank = s2cr & 255;
                    if ((s2cr & (3u << 16)) || bank >= banks) continue;
                    const unsigned base = (pages + bank) * 4096;
                    route[4] = read(0x1000 + 4 * bank);
                    route[5] = read(0x1800 + 4 * bank);
                    route[6] = read(base);
                    route[7] = read(base + 0x30);
                    route[8] = read(base + 0x58);
                }
            }
        }
        smmu->release();
    }
    erhai_send_all(connection, report, sizeof(report));
}

static bool erhai_touch_smmu_sync(IOMemoryMap *smmu, socket_t connection = nullptr)
{
    if (connection) {
        const uint32_t trace[] = { 0x30068, UINT32_MAX };
        erhai_send_all(connection, trace, sizeof(trace));
        IOSleep(16);
    }
    erhai_touch_write(smmu, 0x68, UINT32_MAX);
    if (connection) {
        const uint32_t trace[] = { 0x30070, UINT32_MAX };
        erhai_send_all(connection, trace, sizeof(trace));
        IOSleep(16);
    }
    erhai_touch_write(smmu, 0x70, UINT32_MAX);
    for (unsigned i = 0; i < 2000; ++i) {
        if (!(erhai_touch_reg(smmu, 0x74) & 1)) return true;
        IODelay(10);
    }
    return false;
}

static bool erhai_touch_context_sync(IOMemoryMap *smmu, unsigned bankBase, unsigned asid)
{
    erhai_touch_write(smmu, bankBase + 0x610, asid); // S1_TLBIASID
    erhai_touch_write(smmu, bankBase + 0x7f0, UINT32_MAX);
    for (unsigned i = 0; i < 2000; ++i) {
        if (!(erhai_touch_reg(smmu, bankBase + 0x7f4) & 1)) return true;
        IODelay(10);
    }
    return false;
}

enum class ErhaiGpiLoopback { None, Duplex, Tx, Rx, QueueRx, ConfiguredRx, LongTx };
enum class ErhaiTouchRoutes { GpiOnly, Dual, DualTrim, DualCapture, DualHid };

static int erhai_touch_dma_probe(socket_t connection, bool tracing = false, bool useGpi = false,
                                 ErhaiGpiLoopback loopbackMode = ErhaiGpiLoopback::None,
                                 ErhaiTouchRoutes routes = ErhaiTouchRoutes::GpiOnly,
                                 const uint8_t *firmware = nullptr)
{
    const bool loopback = loopbackMode != ErhaiGpiLoopback::None;
    const bool includeSpiRoute = routes != ErhaiTouchRoutes::GpiOnly;
    const bool readTrim = routes == ErhaiTouchRoutes::DualTrim;
    const bool sendHid = routes == ErhaiTouchRoutes::DualHid;
    const bool captureReports = routes == ErhaiTouchRoutes::DualCapture || sendHid;
    enum { Step, Error, Branch, Firmware, Interface, Slot, Bank, Smr, S2cr, Fault,
           TxBytes, RxBytes, EngineStatus, DmaTx, DmaRx, ResetLow, ResetHigh, Restored,
           InitialSctlr, InitialCbar, InitialCba2r, PlanReject, FaultBefore, Fields };
    const uint16_t stream = useGpi ? 0xb6 : 0xa3;
    constexpr uint32_t voteBit = 1U << 26;
    constexpr uint32_t clockOff = 1U << 31;
    constexpr unsigned page = 4096;
    // Linux allocates the working GPI rings immediately below 4 GiB.
    // The configured-RX control uses the same address range class and leaves
    // space below the 32-bit doorbell boundary for the whole private buffer.
    const uint64_t gpiIova = (includeSpiRoute || loopbackMode == ErhaiGpiLoopback::ConfiguredRx) ?
        0xffff8000 : 0x40000000;
    constexpr unsigned tablePages = 4;
    constexpr unsigned contextOffsets[] = {0x10, 0x20, 0x24, 0x30, 0x38};
    constexpr uint32_t tcr2Fields = (7U << 15) | (1U << 4) | 0xfU; // SEP, AS and PASize.
    // Stage 1 with stage 2 bypass, and Linux's default shareability/memory
    // attributes. Apply only to the private GPI context used by this probe.
    constexpr uint32_t gpiContextAttributes = 0x1f300;
    const unsigned bufferBytes = (useGpi ? 4 : 2) * page;
    static IOBufferMemoryDescriptor *quarantined;
    static IOBufferMemoryDescriptor *quarantinedTables;
    uint32_t report[Fields] = {};
    auto trace = [&](uint32_t point, uint32_t value, bool yield = true) {
        if (!tracing) return;
        const uint32_t record[] = { point, value };
        erhai_send_all(connection, record, sizeof(record));
        if (yield) IOSleep(16);
    };
    if (quarantined || quarantinedTables) {
        report[Error] = uint32_t(-16);
        trace(UINT32_MAX, sizeof(report));
        if (connection) erhai_send_all(connection, report, sizeof(report));
        return -16;
    }

    IOMemoryMap *gcc = erhai_touch_map(0x100000, true, 0x53000);
    IOMemoryMap *smmu = erhai_touch_map(UFS_SMMU_BASE, true, UFS_SMMU_SIZE);
    IOMemoryMap *engine = nullptr;
    IOMemoryMap *wrapper = nullptr;
    IOMemoryMap *pins = nullptr;
    IOBufferMemoryDescriptor *buffer = nullptr;
    IOBufferMemoryDescriptor *tables = nullptr;
    UfsSmmuPlan plan, spiPlan;
    geni_spi spi = {};
    ErhaiTouchGpi gpi;
    gpi.loopback = loopback;
    uint32_t originalPins[4] = {};
    uint32_t savedSmr = 0, savedS2cr = 0, savedCbar = 0, savedCba2r = 0, savedSctlr = 0;
    uint32_t savedSpiSmr = 0;
    unsigned bankBase = 0;
    unsigned pinsChanged = 0;
    bool clockAdded = false, routeChanged = false, spiRouteChanged = false, dmaStarted = false;
    const int error = [&]() -> int {
        if (firmware && !erhai_nt_verify_firmware(firmware)) return -22;
        if (!gcc || !smmu) return -12;
        report[Step] = 1;
        trace(1, 0);
        const auto vote = erhai_touch_reg(gcc, 0x52008);
        if ((erhai_touch_reg(gcc, 0x23140) | erhai_touch_reg(gcc, 0x23144)) & clockOff)
            return -19;
        // R57 measured XO at the RCG. Do not retune a different clock.
        if (erhai_touch_reg(gcc, 0x183e0) != 0x00100000) return -22;
        clockAdded = !(vote & voteBit);
        if (clockAdded) erhai_touch_write(gcc, 0x52008, vote | voteBit);
        for (unsigned i = 0; i < 100; ++i) {
            report[Branch] = erhai_touch_reg(gcc, 0x183cc);
            if (!(report[Branch] & clockOff)) break;
            IODelay(10);
        }
        if (report[Branch] & clockOff) return -110;
        engine = erhai_touch_map(0xa90000, true, 0x4000);
        wrapper = erhai_touch_map(0xac0000);
        if (!loopback) pins = erhai_touch_map(0xf130000, true, 4 * page); // TLMM GPIO 48–51
        if (!engine || !wrapper || (!loopback && !pins)) return -12;
        report[Step] = 2;
        trace(2, report[Branch]);
        spi.se = {engine, erhai_touch_read, erhai_touch_write, erhai_touch_delay};
        spi.wrapper = {wrapper, erhai_touch_read, nullptr, nullptr};
        const int info = geni_spi_read_info(&spi);
        report[Firmware] = spi.info.fw_revision;
        report[Interface] = spi.info.if_disable;
        if (info) return info;
        if (spi.info.protocol != GENI_PROTO_SPI || !spi.info.fifo_disabled ||
            (spi.info.geni_status & (GENI_STATUS_M_ACTIVE | (1U << 12)))) return -16;
        if (useGpi) {
            gpi.registers = erhai_touch_map(0xa10000, true, 5 * erhaiGpiStride);
            if (!gpi.registers) return -12;
            const unsigned instance = gpi.findIdleInstance(loopback || includeSpiRoute ? 1 : 2);
            trace(0xb0, instance);
            if (!instance) return -16;
        }

        const auto options = static_cast<IOOptionBits>(kIODirectionInOut) |
            static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
            static_cast<IOOptionBits>(kIOMemoryMapperNone);
        buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, bufferBytes, page);
        if (useGpi) trace(0xb9, buffer ? 1 : 0);
        if (!buffer) return -12;
        IOByteCount contiguous = 0;
        const auto physical = buffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
        auto *bytes = static_cast<uint8_t *>(buffer->getBytesNoCopy());
        if (!bytes || !physical || (physical & (page - 1)) || contiguous < bufferBytes) return -14;
        const uint64_t dmaAddress = useGpi ? gpiIova : physical;
        if (useGpi) {
            trace(0xe5, uint32_t(dmaAddress));
            gpi.physical = dmaAddress;
            gpi.memory = bytes;
            memset(bytes, 0, bufferBytes);
            erhai_touch_cache<ErhaiTouchCache::CleanInvalidate>(nullptr, bytes, bufferBytes);
        }
        auto read = [&](unsigned offset) { return erhai_touch_reg(smmu, offset); };
        auto write = [&](unsigned offset, uint32_t value) {
            trace(0x10000000 | offset, value);
            erhai_touch_write(smmu, offset, value);
        };
        trace(3, uint32_t(physical >> 32));
        plan = planUfsSmmu(stream, dmaAddress, bufferBytes, read);
        report[Step] = 3;
        report[Slot] = plan.slot;
        report[Bank] = plan.bank;
        report[Smr] = plan.smr;
        report[S2cr] = plan.s2cr;
        if (plan.error) {
            report[PlanReject] = !strcmp(plan.error, "reserved context bank not bypass") ? 1 :
                !strcmp(plan.error, "conflicting UFS stream mapping") ? 2 :
                !strcmp(plan.error, "reserved context bank already used") ? 3 : 4;
            return -16;
        }
        if (plan.reuse) return -16;
        bankBase = (plan.pages + plan.bank) * page;
        savedSctlr = read(bankBase);
        trace(0x31, savedSctlr);
        report[InitialSctlr] = savedSctlr;
        // A disabled bank may still have its reset-default control bits.
        // Preserve them, but refuse any bank with translation active.
        if (savedSctlr & 1) return -16;
        savedSmr = read(0x800 + 4 * plan.slot);
        savedS2cr = read(0xc00 + 4 * plan.slot);
        savedCbar = read(0x1000 + 4 * plan.bank);
        savedCba2r = read(0x1800 + 4 * plan.bank);
        report[InitialCbar] = savedCbar;
        report[InitialCba2r] = savedCba2r;
        report[FaultBefore] = read(0x48);
        if (useGpi) {
            const uint32_t idr2 = read(0x28);
            const unsigned inputBits = ufsSmmuAddressBits(idr2 & 15);
            const unsigned outputCode = (idr2 >> 4) & 15;
            const unsigned outputBits = ufsSmmuAddressBits(outputCode);
            if (inputBits != 36 || !outputBits) return -19;
            tables = IOBufferMemoryDescriptor::inTaskWithOptions(
                kernel_task, options, tablePages * page, page);
            if (!tables) return -12;
            IOByteCount tableContiguous = 0;
            const auto tablePhysical = tables->getPhysicalSegment(
                0, &tableContiguous, kIOMemoryMapperNone);
            auto *entries = static_cast<uint64_t *>(tables->getBytesNoCopy());
            if (!entries || !tablePhysical || (tablePhysical & (page - 1)) ||
                tableContiguous < tablePages * page) return -14;
            const ArmSmmu4KConfig config = {inputBits, outputBits, tablePhysical + page, bufferBytes / page};
            const ArmSmmu4KRange range = {dmaAddress, physical, bufferBytes,
                ArmSmmu4KRead | ArmSmmu4KWrite, 0, 3};
            ArmSmmu4KResult built;
            const auto status = ArmSmmu4KBuild(
                &config, &range, 1, entries + page / sizeof(*entries), tablePages - 1, &built);
            trace(0xba, status);
            if (status || built.start_level != 1 || built.table_pages != tablePages - 1)
                return -14;
            // This SMMU has been observed starting at L0 with a 36-bit TCR.
            // Mirror the active L1 entry into the root so an L1-start walk
            // reaches the same descendants as an L0-start walk.
            const unsigned l1Index = (dmaAddress >> 30) & 0x1ff;
            if (!l1Index || ((dmaAddress + bufferBytes - 1) >> 30) != (dmaAddress >> 30))
                return -14;
            memset(entries, 0, page);
            entries[0] = built.root_physical | 3;
            entries[l1Index] = entries[page / sizeof(*entries) + l1Index];
            erhai_touch_cache<ErhaiTouchCache::Clean>(nullptr, entries, tablePages * page);

            const uint32_t tcr = (64 - inputBits) | (1U << 8) | (1U << 10) |
                (3U << 12) | (1U << 23); // 4K TTBR0, inner-shareable WB walks, TTBR1 off.
            const auto programRoute = [&](const UfsSmmuPlan &route, uint16_t sid, bool &changed) -> int {
                const unsigned base = (route.pages + route.bank) * page;
                const unsigned asid = route.bank + 1; // The local UFS context uses ASID 0.
                const uint32_t contextValues[] = {
                    outputCode | (7U << 15), uint32_t(tablePhysical),
                    uint32_t(tablePhysical >> 32) | (asid << 16), tcr, uint32_t(built.mair)
                };
                if ((read(base) & 1) || (read(base + 0x58) & contextFaultBits)) return -16;
                changed = true;
                write(base, 0);
                write(0x1000 + 4 * route.bank, gpiContextAttributes);
                write(0x1800 + 4 * route.bank, 1);
                for (unsigned i = 0; i < sizeof(contextOffsets) / sizeof(*contextOffsets); ++i) {
                    const unsigned offset = base + contextOffsets[i];
                    write(offset, contextValues[i]);
                    trace(0x20000000 | offset, contextValues[i]);
                    const uint32_t actual = read(offset);
                    trace(0x40000000 | offset, actual);
                    const uint32_t mask = contextOffsets[i] == 0x10 ? tcr2Fields : UINT32_MAX;
                    if ((actual & mask) != (contextValues[i] & mask)) return -5;
                }
                if (!erhai_touch_context_sync(smmu, base, asid)) return -110;
                write(base, 0x27);
                if (read(base) != 0x27 || read(0x1000 + 4 * route.bank) != gpiContextAttributes ||
                    read(0x1800 + 4 * route.bank) != 1) return -5;
                write(0xc00 + 4 * route.slot, route.bank);
                if (read(0xc00 + 4 * route.slot) != route.bank) return -5;
                write(0x800 + 4 * route.slot, 0x80000000U | sid);
                return read(0x800 + 4 * route.slot) == (0x80000000U | sid) ? 0 : -5;
            };
            int routeError = programRoute(plan, stream, routeChanged);
            if (routeError) return routeError;
            if (includeSpiRoute) {
                // The streams have independent contexts and ASIDs. Both
                // reference the same immutable, probe-only page tables.
                constexpr uint16_t spiStream = 0xa3;
                spiPlan = planUfsSmmu(spiStream, dmaAddress, bufferBytes, read);
                if (spiPlan.error || spiPlan.reuse || spiPlan.bank == plan.bank ||
                    spiPlan.slot == plan.slot) return -16;
                savedSpiSmr = read(0x800 + 4 * spiPlan.slot);
                routeError = programRoute(spiPlan, spiStream, spiRouteChanged);
                if (routeError) return routeError;
                trace(0xe6, (spiPlan.bank << 16) | spiPlan.slot);
            }
        } else {
            routeChanged = true;
            if (configureUfsSmmu(plan, stream, read, write)) return -5;
            if (!erhai_touch_smmu_sync(smmu, tracing ? connection : nullptr)) return -110;
        }

        spi.use_dma = 1;
        spi.poll_only = 2;
        spi.poll_limit = 10000;
        spi.dma = {nullptr, erhai_touch_cache<ErhaiTouchCache::Clean>,
            erhai_touch_cache<ErhaiTouchCache::CleanInvalidate>,
            erhai_touch_cache<ErhaiTouchCache::Invalidate>,
            bytes, bytes + page, dmaAddress, dmaAddress + page, page};
        trace(0x40, spi.info.fw_revision);
        int result = geni_spi_init(&spi);
        if (!result) result = geni_spi_setup(&spi, 0, 0);
        const unsigned spiClock = (firmware || captureReports) ? 400000 :
            (useGpi && !loopback ? 100000 : 2400000);
        if (!result) result = geni_spi_set_clock(&spi, spiClock, 19200000, 0);
        if (result) return result;
        report[Step] = 4;
        trace(4, 0);
        for (unsigned i = 0; !loopback && i < 4; ++i) {
            originalPins[i] = erhai_touch_reg(pins, i * page);
            uint32_t drive = (i & 1) ? 1 : 2; // 4 mA MOSI/CS, 6 mA MISO/CLK.
            auto setting = (originalPins[i] & ~0x3ffU) | (drive << 6) | (1U << 2);
            erhai_touch_write(pins, i * page, setting);
            ++pinsChanged;
            if (erhai_touch_reg(pins, i * page) != setting) return -5;
        }
        // Novatek page selector changes only the controller's address latch.
        const uint32_t registerAddress = readTrim ? 0x1fb104 : 0x130960;
        const uint8_t pageSelect[] = {0xff, uint8_t(registerAddress >> 15),
                                     uint8_t(registerAddress >> 7)};
        if (useGpi) {
            erhai_touch_write(engine, SE_IRQ_EN, 0);
            erhai_touch_write(engine, SE_GSI_EVENT_EN, 0x0f);
            trace(0xb1, erhai_touch_reg(engine, SE_GSI_EVENT_EN));
            if (!gpi.start()) return -110;
            trace(0xb2, gpi.read(erhaiGpiEvent));
            const unsigned divider = (erhai_touch_reg(engine, GENI_SER_M_CLK_CFG) >> GENI_CLK_DIV_SHIFT)
                & GENI_CLK_DIV_MAX;
            dmaStarted = true;
            if (captureReports) {
#if ERHAI_HOME_INPUT
                const unsigned startReports = native_touch_hid_reports;
                const unsigned startErrors = __atomic_load_n(&erhai_touch_hid_errors, __ATOMIC_RELAXED);
                const unsigned startContacts = native_touch_edge_stats[1];
                const unsigned startEdgeContacts = native_touch_edge_stats[2];
#if ERHAI_HID_CONFIDENCE
                const unsigned startDispatches = native_touch_edge_stats[4];
                const unsigned startDigitizerDispatches = native_touch_edge_stats[5];
#endif
                if (sendHid) {
#if ERHAI_HID_EARLY
                    trace(0x114, erhai_touch_early_result);
                    trace(0x115, erhai_touch_family_load);
                    trace(0x116, erhai_touch_event_load);
#endif
                    result = erhai_start_hid_input(erhai_touch_device, true);
                    trace(0xfa, uint32_t(result));
#if ERHAI_HID_EARLY
                    trace(0x11b, erhai_touch_family_load);
                    trace(0x11c, erhai_touch_event_load);
                    trace(0x118, 0); // Before sampling.
                    unsigned services = 0;
                    erhai_trace_touch_services(erhai_touch_device, 5, services, trace);
                    trace(0x11a, services);
#endif
                    if (result) return result;
                    if (!erhai_touch_sink) return -19;
#if ERHAI_HID_COLLECTION
                    trace(0x111, erhai_touch_event_driver_state());
                    trace(0x113, erhai_touch_input_report_size());
#endif
                }
#else
                if (sendHid) return -19;
#endif
                uint32_t frames = 0;
#if ERHAI_TOUCH_IRQ_LATCH
                IOMemoryMap *touchIrq = erhai_touch_map(0xf100000 + 162 * 0x1000, true);
                result = -12;
                if (touchIrq) {
                    const uint32_t control = erhai_touch_reg(touchIrq, 0);
                    const uint32_t original = erhai_touch_reg(touchIrq, 8);
                    const uint32_t pending = erhai_touch_reg(touchIrq, 12);
                    trace(0x129, original, false);
                    result = -16;
                    // Only borrow an inactive GPIO input with no queued event.
                    if (!(control & 0x23c) && !(original & 0x11) && !(pending & 1)) {
                        // Falling edge, raw status enabled, interrupt delivery masked.
                        const uint32_t latched = (original & ~0x1fU) | 0x1a;
                        erhai_touch_write(touchIrq, 8, latched);
                        erhai_touch_write(touchIrq, 12, 0);
                        const uint32_t actual = erhai_touch_reg(touchIrq, 8);
                        trace(0x12a, actual, false);
                        result = actual == latched ?
                            erhai_nt_capture_reports(gpi, divider, connection, frames, sendHid, touchIrq) : -5;
                        erhai_touch_write(touchIrq, 8, original);
                        erhai_touch_write(touchIrq, 12, 0);
                        const uint32_t restored = erhai_touch_reg(touchIrq, 8);
                        trace(0x12b, restored, false);
                        if (restored != original) result = -5;
                    }
                    touchIrq->release();
                }
#elif ERHAI_TOUCH_IRQ_GATE
                // Read only the OPD2413 touch IRQ GPIO, without changing pin mux or IRQ configuration.
                IOMemoryMap *touchIrq = erhai_touch_map(0xf100000 + 162 * 0x1000);
                result = touchIrq ? erhai_nt_capture_reports(gpi, divider, connection, frames, sendHid, touchIrq)
                                  : -12;
                OSSafeReleaseNULL(touchIrq);
#else
                result = erhai_nt_capture_reports(gpi, divider, connection, frames, sendHid);
#endif
#if ERHAI_HOME_INPUT
                if (sendHid) {
                    erhai_publish_contacts(nullptr, nullptr, nullptr, 0);
#if ERHAI_HID_EARLY
                    trace(0x118, 1); // After sampling.
                    unsigned services = 0;
                    erhai_trace_touch_services(erhai_touch_device, 5, services, trace);
                    trace(0x11a, services);
#endif
                    trace(0xfb, native_touch_hid_reports - startReports);
                    trace(0xfc, __atomic_load_n(&erhai_touch_hid_errors, __ATOMIC_RELAXED) - startErrors);
                    trace(0xfd, uint32_t(__atomic_load_n(&erhai_touch_hid_result, __ATOMIC_RELAXED)));
                    trace(0x103, native_touch_edge_stats[0]);
                    trace(0x104, native_touch_edge_stats[1] - startContacts);
                    trace(0x105, native_touch_edge_stats[2] - startEdgeContacts);
#if ERHAI_HID_CONFIDENCE
                    trace(0x10f, native_touch_edge_stats[4] - startDispatches);
                    trace(0x110, native_touch_edge_stats[5] - startDigitizerDispatches);
#endif
#if ERHAI_HID_COLLECTION
                    trace(0x112, erhai_touch_event_driver_state());
#endif
                }
#endif
                trace(0xf8, frames);
                report[RxBytes] = frames * ErhaiTouchGpi::maxBufferedTransfer;
                if (!result) report[Step] = 6;
                return result;
            }
            if (firmware) {
                uint8_t info[8] = {};
                result = erhai_nt_load_sram(gpi, divider, firmware, trace, report[TxBytes], info);
                memcpy(&report[ResetLow], info, sizeof(info));
                if (result) return result;
                report[RxBytes] = sizeof(info);
                report[Step] = 6;
                return 0;
            }
            if (loopback) {
                const uint8_t pattern[] = {0x69, 0x96, 0xa5, 0x5a, 0x01, 0x80, 0x3c, 0xc3};
                if (loopbackMode == ErhaiGpiLoopback::LongTx) {
                    uint8_t payload[ErhaiTouchGpi::maxTransmit];
                    for (unsigned i = 0; i < sizeof(payload); ++i)
                        payload[i] = pattern[i % sizeof(pattern)] ^ uint8_t(i / sizeof(pattern));
                    // Three descriptors per transfer. This crosses the end
                    // of the ring and submits subsequent transfers after wrap.
                    constexpr unsigned transfers = ErhaiTouchGpi::channelBytes /
                        (3 * sizeof(ErhaiGpiDescriptor)) + 3;
                    for (unsigned i = 0; i < transfers; ++i) {
                        result = gpi.transfer(payload, nullptr, sizeof(payload), divider);
                        trace(0xe9, i + 1, false);
                        trace(0xea, gpi.txHead, false);
                        if (result) return result;
                        report[TxBytes] += sizeof(payload);
                    }
                    report[Step] = 6;
                    return 0;
                }
                if (loopbackMode == ErhaiGpiLoopback::QueueRx) {
                    trace(0xb6, sizeof(pattern));
                    gpi.submitReceive(sizeof(pattern));
                    trace(0xbe, gpi.rxHead);
                    IOSleep(50);
                    trace(0xcf, gpi.read(erhaiGpiRx + 0x10));
                    trace(0xd0, erhai_touch_reg(engine, SE_GENI_STATUS));
                    report[Step] = 6;
                    return 0;
                }
                const bool loopbackTxOnly = loopbackMode == ErhaiGpiLoopback::Tx;
                const bool configured = loopbackMode == ErhaiGpiLoopback::ConfiguredRx;
                const bool loopbackRxOnly = loopbackMode == ErhaiGpiLoopback::Rx || configured;
                if (configured) {
                    result = gpi.transfer(pattern, nullptr, sizeof(pattern), divider, connection);
                    trace(0xe4, uint32_t(result));
                    if (result) return result;
                    report[TxBytes] = sizeof(pattern);
                }
                uint8_t payload[ErhaiTouchGpi::maxBufferedTransfer];
                uint8_t reply[sizeof(payload)] = {};
                const unsigned length = loopbackMode == ErhaiGpiLoopback::Duplex ?
                    sizeof(payload) : sizeof(pattern);
                for (unsigned i = 0; i < length; ++i)
                    payload[i] = pattern[i % sizeof(pattern)] ^ uint8_t(i / sizeof(pattern));
                const uint8_t dummy[sizeof(pattern)] = {};
                result = gpi.transfer(loopbackRxOnly ? dummy : payload, loopbackTxOnly ? nullptr : reply,
                                      length, divider, connection, loopbackRxOnly, engine,
                                      smmu, bankBase, !configured);
                trace(0xb4, gpi.lastEvent[3]);
                if (result) return result;
                if (!loopbackRxOnly) report[TxBytes] = length;
                report[Step] = 6;
                if (loopbackTxOnly) return 0;
                report[RxBytes] = length;
                memcpy(&report[ResetLow], reply, sizeof(pattern));
                if (loopbackRxOnly) {
                    for (unsigned i = 0; i < length; ++i)
                        if (reply[i] != 0x5a) return 0;
                    return -5;
                }
                return memcmp(payload, reply, length) ? -5 : 0;
            }
            result = gpi.transfer(pageSelect, nullptr, sizeof(pageSelect), divider);
            trace(0xb3, gpi.lastEvent[3]);
            if (result) return result;
            report[TxBytes] = sizeof(pageSelect);
            report[Step] = 5;
            const uint8_t readCommand[8] = {uint8_t(registerAddress & 0x7f)};
            uint8_t readReply[8] = {};
            result = gpi.transfer(readCommand, readReply, sizeof(readReply), divider,
                                  tracing ? connection : nullptr, false, engine,
                                  includeSpiRoute ? smmu : nullptr, bankBase);
            trace(0xb4, gpi.lastEvent[3]);
            if (result) return result;
            report[RxBytes] = sizeof(readReply);
            memcpy(&report[ResetLow], readReply, sizeof(readReply));
            report[Step] = 6;
            if (readTrim) {
                // Compare the six trim bytes captured on this tablet in Ubuntu.
                if (memcmp(readReply + 2, erhaiNtTrim, sizeof(erhaiNtTrim))) return -5;
            } else if (includeSpiRoute) {
                const uint8_t resetState = readReply[2]; // Command and dummy byte precede data.
                trace(0xe8, resetState);
                if (resetState < 0xa0 || resetState > 0xaf) return -5;
            }
            return 0;
        }
        dmaStarted = true;
        trace(0x41, sizeof(pageSelect));
        result = geni_spi_transfer(&spi, pageSelect, nullptr, sizeof(pageSelect), 0);
        if (result) return result;
        report[Step] = 5;
        trace(5, spi.last_tx_in, false);
        if (!erhai_touch_wait_idle(engine, report[EngineStatus])) return -110;
        trace(0x50, report[EngineStatus], false);
        // Keep CS asserted between the read address and the receive clocks.
        // The first received byte is the controller's dummy byte.
        const uint8_t resetAddress = 0x60;
        uint8_t resetReply[8] = {};
        result = geni_spi_transfer(&spi, &resetAddress, nullptr, 1, 1);
        if (result) return result;
        trace(0x51, spi.last_tx_in, false);
        if (!erhai_touch_wait_idle(engine, report[EngineStatus])) return -110;
        trace(0x52, report[EngineStatus], false);
        result = geni_spi_transfer(&spi, nullptr, resetReply + 1, sizeof(resetReply) - 1, 0);
        if (!result && !erhai_touch_wait_idle(engine, report[EngineStatus])) return -110;
        if (!result) {
            report[Step] = 6;
            memcpy(&report[ResetLow], resetReply, sizeof(resetReply));
        }
        return result;
    }();
    report[Error] = uint32_t(error);
    bool gpiStopped = true;
    if (useGpi) {
        if (gpi.issuedCommand) {
            report[EngineStatus] = gpi.read(0x3200);
            report[DmaTx] = gpi.lastEvent[2];
            report[DmaRx] = gpi.lastEvent[3];
            trace(0xb5, report[EngineStatus]);
            gpiStopped = !gpi.commandTimedOut && gpi.stop(trace);
            trace(0xc8, gpiStopped);
        }
        if (engine && gpiStopped && gpi.issuedCommand)
            erhai_touch_write(engine, SE_GSI_EVENT_EN, 0);
    } else {
        report[TxBytes] = spi.last_tx_in;
        report[RxBytes] = spi.last_rx_in;
        if (spi.snap_valid) report[EngineStatus] = spi.snap_status;
        report[DmaTx] = spi.snap_dma_tx;
        report[DmaRx] = spi.snap_dma_rx;
    }
    if (routeChanged) {
        report[Fault] = erhai_touch_reg(smmu, 0x48);
        if (useGpi) {
            trace(0xbb, erhai_touch_reg(smmu, bankBase + 0x58), false);
            trace(0xbc, erhai_touch_reg(smmu, bankBase + 0x60), false);
            trace(0xbd, erhai_touch_reg(smmu, bankBase + 0x68), false);
            if (spiRouteChanged)
                trace(0xe7, erhai_touch_reg(smmu, (spiPlan.pages + spiPlan.bank) * page + 0x58), false);
        }
    }

    // A failed DMA operation can outlive the software timeout. Retain its
    // buffer, clock, pins and route until the temporary boot is restarted.
    if (!gpiStopped || (dmaStarted && ((!useGpi && error) ||
            (erhai_touch_reg(engine, SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE)))) {
        quarantined = buffer;
        quarantinedTables = tables;
        buffer = nullptr;
        tables = nullptr;
    } else {
        for (unsigned i = 0; i < pinsChanged; ++i)
            erhai_touch_write(pins, i * page, originalPins[i]);
        bool synced = true;
        if (routeChanged) {
            const auto restore = [&](unsigned offset, uint32_t value) {
                if (useGpi) trace(0x50000000 | offset, value);
                erhai_touch_write(smmu, offset, value);
            };
            trace(0xc9, savedSmr);
            restore(0x800 + 4 * plan.slot, savedSmr);
            if (useGpi) {
                if (spiRouteChanged) restore(0x800 + 4 * spiPlan.slot, savedSpiSmr);
                const auto detach = [&](const UfsSmmuPlan &route) {
                    const unsigned base = (route.pages + route.bank) * page;
                    restore(base, 0);
                    const bool inactive = !(erhai_touch_reg(smmu, base) & 1) &&
                        !(erhai_touch_reg(smmu, 0x800 + 4 * route.slot) & 0x80000000U);
                    if (!inactive || !erhai_touch_context_sync(smmu, base, route.bank + 1)) return false;
                    const uint32_t fault = erhai_touch_reg(smmu, base + 0x58) & contextFaultBits;
                    if (fault) restore(base + 0x58, fault);
                    // An inactive context must not retain a pointer to freed tables.
                    restore(base + 0x20, 0);
                    restore(base + 0x24, 0);
                    return !erhai_touch_reg(smmu, base + 0x20) &&
                        !erhai_touch_reg(smmu, base + 0x24);
                };
                synced = detach(plan);
                if (spiRouteChanged) synced = detach(spiPlan) && synced;
                trace(0xca, synced);
            } else {
                restore(0xc00 + 4 * plan.slot, savedS2cr);
                restore(0x1000 + 4 * plan.bank, savedCbar);
                restore(0x1800 + 4 * plan.bank, savedCba2r);
                restore(bankBase, savedSctlr);
                synced = erhai_touch_smmu_sync(smmu);
            }
            trace(0xcb, synced);
        }
        if (!synced) {
            quarantined = buffer;
            quarantinedTables = tables;
            buffer = nullptr;
            tables = nullptr;
        }
        if (clockAdded)
            erhai_touch_write(gcc, 0x52008, erhai_touch_reg(gcc, 0x52008) & ~voteBit);
        report[Restored] = synced;
    }
    if (!report[Restored] && !report[Error]) report[Error] = uint32_t(-5);
    OSSafeReleaseNULL(buffer);
    OSSafeReleaseNULL(tables);
    OSSafeReleaseNULL(pins);
    OSSafeReleaseNULL(wrapper);
    OSSafeReleaseNULL(engine);
    OSSafeReleaseNULL(smmu);
    OSSafeReleaseNULL(gcc);
    OSSafeReleaseNULL(gpi.registers);
    trace(UINT32_MAX, sizeof(report));
    if (connection) erhai_send_all(connection, report, sizeof(report));
    return int32_t(report[Error]);
}

#if ERHAI_TOUCH_AUTO
static const uint8_t erhaiTouchFirmware[] = {
#include "ErhaiTouchFirmwareBytes.inc"
};
static_assert(sizeof(erhaiTouchFirmware) == erhaiNtFirmwareSize);

static unsigned erhai_touch_worker_stage;
static int erhai_touch_worker_status;

static void erhai_touch_worker(void *, wait_result_t)
{
    constexpr uint32_t registeredAndOpen = (1u << 28) | (1u << 26);
    bool ready = false;
    for (unsigned attempt = 0; attempt < 1800; ++attempt) {
        if (&native_legacy_frame_count && native_legacy_frame_count >= 12 &&
            (erhai_touch_event_driver_state() & registeredAndOpen) == registeredAndOpen) {
            ready = true;
            break;
        }
        IOSleep(500);
    }
    int status = -110;
    if (ready) {
        __atomic_store_n(&erhai_touch_worker_stage, 2, __ATOMIC_RELAXED);
        status = erhai_touch_dma_probe(nullptr, false, true, ErhaiGpiLoopback::None,
                                       ErhaiTouchRoutes::Dual, erhaiTouchFirmware);
        if (!status) {
            __atomic_store_n(&erhai_touch_worker_stage, 3, __ATOMIC_RELAXED);
            status = erhai_touch_dma_probe(nullptr, false, true, ErhaiGpiLoopback::None,
                                           ErhaiTouchRoutes::DualHid);
        }
    }
    __atomic_store_n(&erhai_touch_worker_status, status, __ATOMIC_RELAXED);
    __atomic_store_n(&erhai_touch_worker_stage, 4, __ATOMIC_RELEASE);
}
#endif
