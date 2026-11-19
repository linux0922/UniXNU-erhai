// OPD2413 QUPv3 wrap1 SE4 read-only handoff diagnostics. Board addresses
// come from sm8750-erhai.dts and the named GCC branches in gcc-sm8750.c.
static IOMemoryMap *erhai_touch_map(IOPhysicalAddress address, bool writable = false,
                                   IOByteCount length = 0x1000)
{
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withPhysicalAddress(
        address, length, writable ? kIODirectionInOut : kIODirectionIn);
    if (!memory) return nullptr;
    unsigned options = kIOMapAnywhere | kIOMapInhibitCache;
    if (!writable) options |= kIOMapReadOnly;
    IOMemoryMap *mapping = memory->map(options);
    memory->release();
    return mapping;
}

static uint32_t erhai_touch_reg(IOMemoryMap *mapping, unsigned offset)
{
    return *reinterpret_cast<volatile const uint32_t *>(mapping->getVirtualAddress() + offset);
}

[[maybe_unused]] static void erhai_touch_probe(socket_t connection, bool testClock)
{
    enum { Stage, Branch, MasterAhb, SlaveAhb, Vote, ClockConfig, Firmware, Interface,
           EngineStatus, MainClock, ClockSelect, BranchDuring, VoteAfter, Fields };
    constexpr uint32_t off = 1U << 31;
    constexpr uint32_t se4Vote = 1U << 26;
    uint32_t report[Fields] = {};
    IOMemoryMap *clock = erhai_touch_map(0x118000);
    IOMemoryMap *ahb = erhai_touch_map(0x123000);
    IOMemoryMap *votes = erhai_touch_map(0x152000, testClock);
    volatile uint32_t *voteRegister = nullptr;
    if (clock && ahb && votes) {
        report[Stage] = 1;
        report[Branch] = erhai_touch_reg(clock, 0x3cc);
        report[ClockConfig] = erhai_touch_reg(clock, 0x3e0);
        report[MasterAhb] = erhai_touch_reg(ahb, 0x140);
        report[SlaveAhb] = erhai_touch_reg(ahb, 0x144);
        report[Vote] = erhai_touch_reg(votes, 0x008);
        report[BranchDuring] = report[Branch];
        const bool ahbOnline = !((report[MasterAhb] | report[SlaveAhb]) & off);
        if (testClock && ahbOnline && (report[Branch] & off) && !(report[Vote] & se4Vote)) {
            voteRegister = reinterpret_cast<volatile uint32_t *>(votes->getVirtualAddress() + 0x008);
            *voteRegister = report[Vote] | se4Vote;
            __asm__ volatile("dsb sy" ::: "memory");
            for (unsigned i = 0; i < 100; ++i) {
                report[BranchDuring] = erhai_touch_reg(clock, 0x3cc);
                if (!(report[BranchDuring] & off)) break;
                IODelay(10);
            }
        }
    }

    // Skip SE register reads when GCC reports any required clock as off.
    if (report[Stage] && !((report[BranchDuring] | report[MasterAhb] | report[SlaveAhb]) & off)) {
        IOMemoryMap *engine = erhai_touch_map(0x00a90000);
        if (engine) {
            report[Stage] = 2;
            report[Firmware] = erhai_touch_reg(engine, 0x68);
            report[Interface] = erhai_touch_reg(engine, 0x64);
            report[EngineStatus] = erhai_touch_reg(engine, 0x40);
            report[MainClock] = erhai_touch_reg(engine, 0x48);
            report[ClockSelect] = erhai_touch_reg(engine, 0x7c);
            engine->release();
        }
    }
    if (voteRegister) {
        *voteRegister = *voteRegister & ~se4Vote;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    if (votes) report[VoteAfter] = erhai_touch_reg(votes, 0x008);
    OSSafeReleaseNULL(votes);
    OSSafeReleaseNULL(ahb);
    OSSafeReleaseNULL(clock);
    erhai_send_all(connection, report, testClock ? sizeof(report) : BranchDuring * sizeof(uint32_t));
}
