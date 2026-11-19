// A temporary Home-button source for a wired USB boot. The original HID
// family consumes standard press and release reports from NativeHidInput.
#include <libkern/c++/OSKext.h>
#include <mach/kmod.h>
#if ERHAI_HID_COLLECTION
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#endif

extern kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void erhai_hid_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);

static unsigned erhai_home_pending;
static unsigned erhai_home_down, erhai_home_up;
static int erhai_home_result[2] = { kIOReturnNotReady, kIOReturnNotReady };
static IOService *erhai_home_device;
static IOService *erhai_touch_device;
static void (*erhai_touch_sink)(void *);
static void *erhai_touch_sink_context;
static unsigned erhai_touch_hid_errors;
static int erhai_touch_hid_result = kIOReturnNotReady;

#if ERHAI_HID_EARLY
static uint32_t erhai_touch_early_result = uint32_t(kIOReturnNotReady);
static uint32_t erhai_touch_family_load = uint32_t(kIOReturnNotReady);
static uint32_t erhai_touch_event_load = uint32_t(kIOReturnNotReady);
#endif

#if ERHAI_HID_IDENTITY
static void erhai_publish_touch_identity();
#endif

extern "C" {
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
#if ERHAI_HID_CONFIDENCE
extern volatile uint32_t native_touch_edge_stats[6];
#else
extern volatile uint32_t native_touch_edge_stats[4];
#endif

void native_hid_live_checkpoint(IOService *device)
{
#if ERHAI_HID_IDENTITY
    if (device == erhai_touch_device) erhai_publish_touch_identity();
#else
    (void)device;
#endif
}
void native_touch_set_sink(void (*sink)(void *), void *context)
{
    erhai_touch_sink = sink;
    erhai_touch_sink_context = context;
}
bool native_hid_input_requested(void)
{
    return __atomic_exchange_n(&erhai_home_pending, 0, __ATOMIC_ACQ_REL) != 0;
}
void native_hid_input_observed(unsigned event, int result)
{
    if (event == 6) {
        __atomic_store_n(&erhai_touch_hid_result, result, __ATOMIC_RELAXED);
        if (result) __atomic_add_fetch(&erhai_touch_hid_errors, 1, __ATOMIC_RELAXED);
        return;
    }
    if (event != 4 && event != 5) return;
    __atomic_store_n(&erhai_home_result[event - 4], result, __ATOMIC_RELAXED);
    __atomic_add_fetch(event == 4 ? &erhai_home_down : &erhai_home_up, 1, __ATOMIC_RELEASE);
}
}

static IOReturn erhai_start_hid_input(IOService *&published, bool touch)
{
    if (published) return kIOReturnSuccess;
    const int family = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
#if ERHAI_HID_EARLY
    if (touch) erhai_touch_family_load = uint32_t(family);
#endif
    if (family != KERN_SUCCESS) return family;
    const auto eventLoad = OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver", true, false,
                                                        kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
#if ERHAI_HID_EARLY
    if (touch) erhai_touch_event_load = uint32_t(eventLoad);
#else
    (void)eventLoad;
#endif
    OSDictionary *match = IOService::serviceMatching("IOHIDResource");
    IOService *resource = match ? IOService::waitForMatchingService(match, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(match);
    if (!resource) return kIOReturnNotReady;
    const OSSymbol *name = OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *original = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!original || original->getClassSize() != 208) {
        resource->release();
        return kIOReturnUnsupported;
    }
    static bool attempted, registered;
    if (!attempted) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (handle) {
            attempted = true;
            erhai_hid_init();
            registered = OSMetaClass::postModLoad(handle) == KERN_SUCCESS;
        }
    }
    IOService *device = registered ?
        (touch ? native_hid_touch_allocate() : native_hid_input_allocate()) : nullptr;
    const bool attached = device && device->metaCast("IOHIDDevice") && device->init() &&
                          device->attach(resource);
    if (attached && device->start(resource)) {
        published = device;
#if ERHAI_HID_IDENTITY
        if (touch) erhai_publish_touch_identity();
#endif
    } else {
        if (attached) device->detach(resource);
        OSSafeReleaseNULL(device);
    }
    resource->release();
    return published ? kIOReturnSuccess : kIOReturnNotReady;
}

static void erhai_publish_contacts(const int *xs, const int *ys, const int *ids, unsigned count)
{
    __atomic_add_fetch(&native_touch_seq, 1, __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < count; ++i) {
        native_touch_xs[i] = xs[i];
        native_touch_ys[i] = ys[i];
        native_touch_ids[i] = ids[i];
    }
    native_touch_count = count;
    __atomic_add_fetch(&native_touch_seq, 1, __ATOMIC_RELEASE);
    if (erhai_touch_sink) erhai_touch_sink(erhai_touch_sink_context);
}

#if ERHAI_HID_COLLECTION
static IORegistryEntry *erhai_find_touch_event_driver(IORegistryEntry *parent, unsigned depth)
{
    if (!parent || !depth) return nullptr;
    if (parent->metaCast("IOHIDEventDriver")) {
        parent->retain();
        return parent;
    }
    OSIterator *children = parent->getChildIterator(gIOServicePlane);
    if (!children) return nullptr;
    IORegistryEntry *driver = nullptr;
    while (OSObject *child = children->getNextObject()) {
        driver = erhai_find_touch_event_driver(OSDynamicCast(IORegistryEntry, child), depth - 1);
        if (driver) break;
    }
    children->release();
    return driver;
}

#if ERHAI_HID_IDENTITY
static void erhai_set_digitizer_identity(IORegistryEntry *entry)
{
    OSObject *pageObject = entry->copyProperty("PrimaryUsagePage");
    OSObject *usageObject = entry->copyProperty("PrimaryUsage");
    const auto *page = OSDynamicCast(OSNumber, pageObject);
    const auto *usage = OSDynamicCast(OSNumber, usageObject);
    if (!page || page->unsigned32BitValue() != 0x0d)
        entry->setProperty("PrimaryUsagePage", 0x0dULL, 32);
    if (!usage || usage->unsigned32BitValue() != 4)
        entry->setProperty("PrimaryUsage", 4ULL, 32);
    OSSafeReleaseNULL(usageObject);
    OSSafeReleaseNULL(pageObject);
    if (entry->getProperty("Built-In") != kOSBooleanTrue)
        entry->setProperty("Built-In", kOSBooleanTrue);
    if (entry->getProperty("DisplayIntegrated") != kOSBooleanTrue)
        entry->setProperty("DisplayIntegrated", kOSBooleanTrue);
}

static void erhai_publish_touch_identity()
{
    // Publish on the owned event service and its interface after matching.
    IORegistryEntry *driver = erhai_find_touch_event_driver(erhai_touch_device, 5);
    if (!driver) return;
    IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane);
    if (interface && interface->metaCast("IOHIDInterface"))
        erhai_set_digitizer_identity(interface);
    erhai_set_digitizer_identity(driver);
    driver->release();
}
#endif

static uint32_t erhai_touch_event_driver_state(IORegistryEntry *driver)
{
    if (!driver) return 0;
    OSObject *pageObject = driver->copyProperty("PrimaryUsagePage");
    OSObject *usageObject = driver->copyProperty("PrimaryUsage");
    const auto *page = OSDynamicCast(OSNumber, pageObject);
    const auto *usage = OSDynamicCast(OSNumber, usageObject);
    // Presence, serialization success and receive timestamp occupy the high bits.
    uint32_t state = 1u << 31;
    if (page) state |= uint32_t(page->unsigned8BitValue()) << 8;
    if (usage) state |= usage->unsigned8BitValue();
    IOService *service = OSDynamicCast(IOService, driver);
    if (service) {
        const IOOptionBits flags = service->getState();
        if (flags & kIOServiceRegisteredState) state |= 1u << 28;
        if (flags & kIOServiceMatchedState) state |= 1u << 27;
        if (flags & kIOServiceInactiveState) state |= 1u << 23;
        auto *interface = OSDynamicCast(IOService, driver->getParentEntry(gIOServicePlane));
        if (interface && interface->metaCast("IOHIDInterface")) {
            state |= 1u << 24;
            if (interface->isOpen(service)) state |= 1u << 26;
            if (interface->isOpen()) state |= 1u << 25;
        }
    }
    OSObject *debug = driver->copyProperty("DebugState");
    OSSerialize *serialized = debug ? OSSerialize::withCapacity(256) : nullptr;
    if (serialized && debug->serialize(serialized)) {
        state |= 1u << 30;
        constexpr char key[] = "<key>LastReportTime</key>";
        constexpr size_t keyLength = sizeof(key) - 1;
        const size_t length = serialized->getLength();
        const char *data = serialized->text();
        for (size_t offset = 0; offset + keyLength <= length; ++offset) {
            if (memcmp(data + offset, key, keyLength)) continue;
            state |= 1u << 29;
            break;
        }
    }
    OSSafeReleaseNULL(serialized);
    OSSafeReleaseNULL(debug);
    OSSafeReleaseNULL(usageObject);
    OSSafeReleaseNULL(pageObject);
    return state;
}

static uint32_t erhai_touch_event_driver_state()
{
    IORegistryEntry *driver = erhai_find_touch_event_driver(erhai_touch_device, 5);
    const uint32_t state = erhai_touch_event_driver_state(driver);
    OSSafeReleaseNULL(driver);
    return state;
}

#if ERHAI_HID_EARLY
template <class Trace>
static void erhai_trace_touch_services(IORegistryEntry *entry, unsigned depth, unsigned &count,
                                       const Trace &trace)
{
    if (!entry || !depth) return;
    if (entry->metaCast("IOHIDEventDriver")) {
        trace(0x119, erhai_touch_event_driver_state(entry));
        ++count;
    }
    OSIterator *children = entry->getChildIterator(gIOServicePlane);
    if (!children) return;
    while (OSObject *child = children->getNextObject())
        erhai_trace_touch_services(OSDynamicCast(IORegistryEntry, child), depth - 1, count, trace);
    children->release();
}
#endif

static uint32_t erhai_touch_input_report_size()
{
    OSObject *property = erhai_touch_device->copyProperty("MaxInputReportSize");
    const auto *size = OSDynamicCast(OSNumber, property);
    const uint32_t bytes = size ? size->unsigned32BitValue() : UINT32_MAX;
    OSSafeReleaseNULL(property);
    return bytes;
}
#endif

static void erhai_home_report(socket_t conn, bool press)
{
    const int status = !press ? kIOReturnSuccess : erhai_start_hid_input(erhai_home_device, false);
    if (press && status == kIOReturnSuccess) {
        __atomic_store_n(&erhai_home_pending, 1, __ATOMIC_RELEASE);
    }
    const uint32_t report[] = {
        uint32_t(status), unsigned(erhai_home_device != nullptr),
        __atomic_load_n(&erhai_home_pending, __ATOMIC_ACQUIRE),
        __atomic_load_n(&erhai_home_down, __ATOMIC_ACQUIRE),
        __atomic_load_n(&erhai_home_up, __ATOMIC_ACQUIRE),
        uint32_t(__atomic_load_n(&erhai_home_result[0], __ATOMIC_RELAXED)),
        uint32_t(__atomic_load_n(&erhai_home_result[1], __ATOMIC_RELAXED)),
    };
    erhai_send_all(conn, report, sizeof(report));
}
