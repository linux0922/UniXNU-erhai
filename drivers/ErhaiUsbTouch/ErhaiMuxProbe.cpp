// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device-side USBMux transport plus a read-only block device backed by the host.
//
// Phase 1: fixed handshake probe (unchanged).
// Phase 2: block requests, configurable size, per-volume.
// Phase 3: a host block connection is adopted as the backing store for an
//          IOBlockStorageDevice, so APFS can see the OS/SystemOS volumes.

#include <sys/kpi_socket.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSSymbol.h>

/* Enable only with the checked original IOStorageFamily bindings. */
#ifndef ERHAI_WITH_STORAGE
#define ERHAI_WITH_STORAGE 0
#endif

#if ERHAI_WITH_STORAGE
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/OSKextLib.h>
#if ERHAI_USB_ROOT
#include <IOKit/IOBSD.h>
#include <libkern/c++/OSNumber.h>
#endif
#if ERHAI_VERIFY_STORAGE
#include "verified_root.h"
extern "C" const vr_crypto vr_xnu_crypto;
extern "C" const uint8_t erhai_os_policy_index[];
extern "C" const uint8_t erhai_systemos_policy_index[];
#endif
extern kmod_info_t g_kernel_kmod_info;
extern "C" void erhai_storage_static_init(void) __asm__("__GLOBAL__sub_I_ErhaiMuxProbe.cpp");
#if ERHAI_KEYSTORE_EVENTS
#include "NativeKeyStoreEvents.cpp"
static uint32_t erhai_keystore_state, erhai_keystore_opens, erhai_keystore_calls;
extern "C" bool native_aks_state_enabled(void) { return true; }
extern "C" uint32_t native_aks_lock_state(void) { return 0; }
extern "C" void native_aks_call(uint32_t, int32_t) { __atomic_add_fetch(&erhai_keystore_calls, 1, __ATOMIC_RELAXED); }
extern "C" void native_aks_opened(void) { __atomic_add_fetch(&erhai_keystore_opens, 1, __ATOMIC_RELAXED); }
extern "C" void native_keyevents_open_refused(void) {}
#endif
#endif
#if ERHAI_AUX_CHECK
extern "C" int erhai_aux_status[2];
#endif

#define ERHAI_PROBE_PORT 62078
#define ERHAI_MAX_BLOCK (1024u * 1024u)
/* Readahead can be disabled at build time (-DERHAI_READAHEAD=0) so a
 * signature-path candidate can be measured without the transport change. */
#ifndef ERHAI_READAHEAD
#define ERHAI_READAHEAD 1
#endif
#define ERHAI_MAX_REQUESTS 64
#define ERHAI_OS_BYTES 6077580800ull      /* whole GPT disk, 044-76040-038 */
#define ERHAI_SYSTEMOS_BYTES 3303014400ull /* decoded 044-76034-038 */

static const char erhai_probe_request[] = "ERHAI-PROBE?";
static const char erhai_probe_reply[] = "ERHAI-MUX-OK";

static unsigned char erhai_block_buffer[ERHAI_MAX_BLOCK];

struct __attribute__((packed)) ErhaiConfig {
	char magic[4];
	uint32_t block_size;
	uint32_t count;
	uint32_t volume;
};

struct __attribute__((packed)) ErhaiBlockRequest {
	char magic[4];
	uint32_t volume;
	uint64_t offset;
	uint32_t length;
};

struct __attribute__((packed)) ErhaiBlockHeader {
	char magic[4];
	uint32_t status;
	uint32_t length;
};

struct __attribute__((packed)) ErhaiVerify {
	char magic[4];
	uint32_t status;
	uint32_t checksum;
	uint32_t length;
	unsigned char first[4];
};

/* ---- block transport state ------------------------------------------------ */

static socket_t erhai_transport __attribute__((unused)) = NULL;
static IOLock *erhai_transport_lock __attribute__((unused)) = NULL;
static uint32_t erhai_transport_ready __attribute__((unused)) = 0;
static unsigned char erhai_transport_buffer[ERHAI_MAX_BLOCK] __attribute__((unused));

#if ERHAI_WITH_STORAGE
extern "C" void boot_progress_root(const char *, const char *, uint64_t) __attribute__((weak_import));
extern "C" void erhai_boot_progress_snapshot(char *, unsigned int) __attribute__((weak_import));
extern "C" void erhai_boot_usb_status(unsigned, unsigned, uint64_t) __attribute__((weak_import));
extern "C" void erhai_boot_quiet_console(void) __attribute__((weak_import));
extern "C" volatile uint32_t native_legacy_stage __attribute__((weak_import));
extern "C" volatile uint32_t native_legacy_client_calls __attribute__((weak_import));
extern "C" volatile uint32_t native_legacy_frame_count __attribute__((weak_import));
extern "C" volatile int native_legacy_client_status __attribute__((weak_import));
extern "C" volatile int native_legacy_client_pid __attribute__((weak_import));
extern "C" volatile uint32_t native_legacy_frame_kind[32] __attribute__((weak_import));
extern "C" volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak_import));
extern "C" volatile uint32_t native_legacy_swap_rejected_count __attribute__((weak_import));
extern "C" unsigned native_legacy_clients_dump(char *, unsigned) __attribute__((weak_import));
#if ERHAI_LIVE_UI_PROCESSES
struct proc;
using ErhaiProcCallback = int (*)(struct proc *, void *);
extern "C" void proc_iterate(unsigned, ErhaiProcCallback, void *, ErhaiProcCallback, void *);
extern "C" char *proc_best_name(struct proc *);
extern "C" int proc_pid(struct proc *);
extern "C" void proc_name(int, char *, int);
#if ERHAI_UI_THREADS
extern "C" void boot_process_capture(void);
extern "C" void boot_process_snapshot(void);
extern "C" char boot_process_rows[21][120];
static uint32_t erhai_thread_capture_pending;

static void erhai_capture_ui_threads(void *, wait_result_t)
{
	boot_process_capture();
	__atomic_store_n(&erhai_thread_capture_pending, 0, __ATOMIC_RELEASE);
	thread_terminate(current_thread());
}
#endif

struct ErhaiUIProcesses {
	const char *const *names;
	int *pids;
	size_t count;
};

static int erhai_record_ui_process(struct proc *process, void *context)
{
	auto &info = *static_cast<ErhaiUIProcesses *>(context);
	const char *name = proc_best_name(process);
	for (size_t index = 0; name && index < info.count; index++) {
		if (strcmp(name, info.names[index]) == 0) {
			info.pids[index] = proc_pid(process);
			break;
		}
	}
	return 0; /* PROC_RETURNED: the iterator releases its process reference. */
}

static void erhai_append_ui_status(char *report, size_t capacity)
{
	const char *const names[] = { "SpringBoard", "backboardd", "chronod", "liveactivitiesd", "runningboardd" };
	int pids[sizeof(names) / sizeof(names[0])] = {};
	ErhaiUIProcesses processes = { names, pids, sizeof(pids) / sizeof(pids[0]) };
	/* PROC_ALLPROCLIST | PROC_NOWAITTRANS */
	proc_iterate((1u << 0) | (1u << 2), erhai_record_ui_process, &processes, NULL, NULL);
	size_t used = strlen(report);
	snprintf(report + used, capacity - used, "LIVE SpringBoard=%d backboardd=%d chronod=%d liveactivitiesd=%d runningboardd=%d\n",
	    pids[0], pids[1], pids[2], pids[3], pids[4]);
	if (&native_legacy_client_pid && &native_legacy_frame_count && native_legacy_frame_kind &&
	    &native_legacy_swap_accepted_count && &native_legacy_swap_rejected_count) {
		const int pid = native_legacy_client_pid;
		char name[33] = {};
		if (pid > 0) proc_name(pid, name, sizeof(name));
		const unsigned retained = MIN(native_legacy_frame_count, 32u);
		unsigned copies = 0, fills = 0;
		for (unsigned index = 0; index < retained; index++) {
			copies += native_legacy_frame_kind[index] == 0;
			fills += native_legacy_frame_kind[index] == 1;
		}
		used = strlen(report);
		snprintf(report + used, capacity - used, "PRESENT last_client=%d/%s swaps_ok=%u rejected=%u first_copies=%u first_fills=%u\n",
		    pid, name, native_legacy_swap_accepted_count, native_legacy_swap_rejected_count, copies, fills);
	}
}
#endif
/* The caller holds erhai_transport_lock while updating the last read error. */
static const char *erhai_read_error;
static uint64_t erhai_read_error_offset;
static errno_t erhai_read_errno;
static char erhai_last_read_failure[192];

static bool erhai_read_failed(const char *reason, uint64_t offset, errno_t error = 0)
{
	erhai_read_error = reason;
	erhai_read_error_offset = offset;
	erhai_read_errno = error;
	return false;
}
#endif

#if ERHAI_VERIFY_STORAGE
static const uint64_t erhai_disk_bytes[] = { ERHAI_OS_BYTES, ERHAI_SYSTEMOS_BYTES };
static constexpr uint32_t erhai_policy_page_bytes = 16 * 1024;
static constexpr uint32_t erhai_hash_bytes = 32;
struct ErhaiPolicyPage {
	uint64_t index;
	bool valid;
	uint8_t hashes[erhai_policy_page_bytes];
};
#ifndef ERHAI_POLICY_CACHE_PAGES
#define ERHAI_POLICY_CACHE_PAGES 4
#endif
static ErhaiPolicyPage erhai_policy_cache[2][ERHAI_POLICY_CACHE_PAGES];

#if ERHAI_VERIFIED_CACHE
static constexpr unsigned erhai_cache_sets = 2048;
static constexpr unsigned erhai_cache_ways = 4;
static constexpr unsigned erhai_cache_max_read = 65536;
struct ErhaiCachedBlock {
	uint64_t key;
	uint64_t age;
};
static ErhaiCachedBlock erhai_cache_entries[erhai_cache_sets][erhai_cache_ways];
static uint8_t *erhai_cache_data;
static uint64_t erhai_cache_age, erhai_cache_hits, erhai_cache_misses, erhai_cache_hit_bytes;

/* Held under erhai_transport_lock. Age zero marks an empty slot. */
static ErhaiCachedBlock *erhai_cache_entry(uint64_t key, bool insert)
{
	const unsigned set = (unsigned)((key ^ (key >> 21) ^ (key >> 11)) & (erhai_cache_sets - 1));
	auto &entries = erhai_cache_entries[set];
	ErhaiCachedBlock *oldest = &entries[0];
	for (auto &entry : entries) {
		if (entry.age && entry.key == key) {
			return &entry;
		}
		if (entry.age < oldest->age) {
			oldest = &entry;
		}
	}
	return insert ? oldest : NULL;
}
	
static bool erhai_cache_copy(unsigned volume, uint64_t offset, uint32_t length, bool insert)
{
	for (uint32_t at = 0; at < length; at += VR_BLOCK_BYTES) {
		const uint64_t key = ((offset + at) / VR_BLOCK_BYTES << 1) | volume;
		auto *entry = erhai_cache_entry(key, insert);
		if (!entry) {
			return false;
		}
		const size_t set = (key ^ (key >> 21) ^ (key >> 11)) & (erhai_cache_sets - 1);
		const size_t slot = set * erhai_cache_ways + (entry - erhai_cache_entries[set]);
		uint8_t *cached = erhai_cache_data + slot * VR_BLOCK_BYTES;
		if (insert) {
			memcpy(cached, erhai_transport_buffer + at, VR_BLOCK_BYTES);
			entry->key = key;
		} else {
			memcpy(erhai_transport_buffer + at, cached, VR_BLOCK_BYTES);
		}
		entry->age = ++erhai_cache_age;
	}
	return true;
}
#endif
#endif

__attribute__((unused)) static uint32_t erhai_fnv1a(const unsigned char *bytes, uint32_t length)
{
	uint32_t hash = 2166136261u;
	for (uint32_t i = 0; i < length; ++i) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static bool erhai_recv_all(socket_t so, void *buffer, uint32_t length, errno_t *error = NULL)
{
	unsigned char *cursor = (unsigned char *)buffer;
	uint32_t done = 0;
	while (done < length) {
		struct iovec iov;
		struct msghdr msg;
		size_t received = 0;

		iov.iov_base = cursor + done;
		iov.iov_len = length - done;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		errno_t err = sock_receive(so, &msg, 0, &received);
		done += (uint32_t)received;
		if (err == EINTR) {
			continue;
		}
		if (err != 0 || received == 0) {
			if (error) {
				*error = err;
			}
			return false;
		}
	}
	return true;
}

static bool erhai_send_all(socket_t so, const void *buffer, uint32_t length, errno_t *error = NULL)
{
	const unsigned char *cursor = (const unsigned char *)buffer;
	uint32_t done = 0;
	while (done < length) {
		struct iovec iov;
		struct msghdr msg;
		size_t sent = 0;

		iov.iov_base = (void *)(cursor + done);
		iov.iov_len = length - done;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		errno_t err = sock_send(so, &msg, 0, &sent);
		done += (uint32_t)sent;
		if (err == EINTR) {
			continue;
		}
		if (err != 0 || sent == 0) {
			if (error) {
				*error = err;
			}
			return false;
		}
	}
	return true;
}

#if ERHAI_HOME_INPUT
#include "ErhaiHomeInput.inc"
#endif
#if ERHAI_TOUCH_PROBE
#include "ErhaiTouchProbe.inc"
#if ERHAI_TOUCH_DMA
#include "ErhaiTouchDma.inc"
#endif
#endif

/* One read from the host. Caller holds erhai_transport_lock. */
#if ERHAI_WITH_STORAGE
__attribute__((unused)) static bool erhai_usb_read(uint32_t volume, uint64_t offset, uint32_t length, void *out)
{
	struct ErhaiBlockRequest request;
	struct ErhaiBlockHeader header;
	errno_t error = 0;

	if (length == 0 || length > ERHAI_MAX_BLOCK || !erhai_transport) {
		return erhai_read_failed("USB invalid request", offset);
	}
	memcpy(request.magic, "EBK1", 4);
	request.volume = volume;
	request.offset = offset;
	request.length = length;
	if (erhai_boot_usb_status) erhai_boot_usb_status(1, volume, offset);
	if (!erhai_send_all(erhai_transport, &request, sizeof(request), &error)) {
		return erhai_read_failed("USB send request", offset, error);
	}
	if (erhai_boot_usb_status) erhai_boot_usb_status(2, volume, offset);
	if (!erhai_recv_all(erhai_transport, &header, sizeof(header), &error)) {
		return erhai_read_failed("USB receive header", offset, error);
	}
	if (memcmp(header.magic, "EBK1", 4) != 0 || header.status != 0 || header.length != length) {
		return erhai_read_failed("USB invalid reply", offset);
	}
	if (erhai_boot_usb_status) erhai_boot_usb_status(3, volume, offset);
	if (!erhai_recv_all(erhai_transport, out, length, &error)) {
		return erhai_read_failed("USB receive data", offset, error);
	}
	if (erhai_boot_usb_status) erhai_boot_usb_status(4, volume, offset);
	return true;
}
#if ERHAI_SYSTEMOS_RAM
#include "ErhaiSystemOsRam.inc"
#if ERHAI_OS_WORKING_SET
#include "ErhaiOsWorkingSet.inc"
#endif
#endif

#if ERHAI_LOCAL_ROOT
extern "C" int erhai_read_local_volume(uint32_t volume, uint64_t offset, uint32_t length, void *out);
#endif

__attribute__((unused)) static bool erhai_transport_read(uint32_t volume, uint64_t offset, uint32_t length, void *out)
{
#if ERHAI_LOCAL_ROOT
	const int status = erhai_read_local_volume(volume, offset, length, out);
	return !status || erhai_read_failed("local ext4 read", offset, status);
#else
#if ERHAI_OS_WORKING_SET
	if (volume == 0 && os_resident) {
		return erhai_os_cached_read(offset, length, out);
	}
	if (volume == 2 && os_resident) {
		if (offset >= os_manifest_bytes || length > os_manifest_bytes - offset) {
			return erhai_read_failed("OS RAM manifest read range", offset);
		}
		memcpy(out, os_manifest + offset, length);
		return true;
	}
#endif
#if ERHAI_SYSTEMOS_RAM
	if (volume == 1 && systemos_resident) {
		return erhai_systemos_ram_read(offset, length, out);
	}
	if (volume == 3 && systemos_resident) {
		if (offset >= systemos_manifest_bytes || length > systemos_manifest_bytes - offset) {
			return erhai_read_failed("SystemOS RAM manifest read range", offset);
		}
		memcpy(out, systemos_manifest + offset, length);
		return true;
	}
#endif
	return erhai_usb_read(volume, offset, length, out);
#endif
}
#endif

#if ERHAI_VERIFY_STORAGE
#if ERHAI_LOCAL_ROOT
extern "C" void erhai_local_sha256(const void *, size_t, uint8_t *);
#endif
static bool erhai_storage_hash(const void *data, size_t bytes, uint8_t *digest)
{
#if ERHAI_LOCAL_ROOT
	erhai_local_sha256(data, bytes, digest);
	return true;
#else
	return vr_xnu_crypto.sha256(data, bytes, digest);
#endif
}

/* Each page of hashes is independently pinned by the image, after the
 * host build checks the signed manifest. Never cache unauthenticated hashes. */
static const uint8_t *erhai_block_hash(unsigned volume, uint64_t block)
{
	const uint64_t count = (erhai_disk_bytes[volume] + VR_BLOCK_BYTES - 1) / VR_BLOCK_BYTES;
	if (block >= count) {
		return NULL;
	}
	const uint64_t hash = block * erhai_hash_bytes;
	const uint64_t index = hash / erhai_policy_page_bytes;
	auto &page = erhai_policy_cache[volume][index % ERHAI_POLICY_CACHE_PAGES];
	if (!page.valid || page.index != index) {
		page.valid = false;
		const uint64_t offset = index * erhai_policy_page_bytes;
		const uint32_t length = (uint32_t)MIN(count * erhai_hash_bytes - offset, erhai_policy_page_bytes);
		const uint8_t *pinned = volume == 0 ? erhai_os_policy_index : erhai_systemos_policy_index;
		uint8_t digest[erhai_hash_bytes];
		if (!erhai_transport_read(volume + 2, VR_HEADER_BYTES + VR_SIGNATURE_BYTES + offset, length, page.hashes)) {
			return NULL;
		}
		if (!erhai_storage_hash(page.hashes, length, digest) ||
		    memcmp(digest, pinned + index * erhai_hash_bytes, sizeof(digest))) {
			erhai_read_failed("USB manifest page hash", offset);
			return NULL;
		}
		page.index = index;
		page.valid = true;
	}
	return page.hashes + hash % erhai_policy_page_bytes;
}

static bool erhai_read_verified(unsigned volume, uint64_t offset, uint32_t length)
{
#if ERHAI_VERIFIED_CACHE
	const bool cacheable = erhai_cache_data && length <= erhai_cache_max_read &&
	    ((offset | length) & (VR_BLOCK_BYTES - 1)) == 0;
	if (cacheable) {
		if (erhai_cache_copy(volume, offset, length, false)) {
			++erhai_cache_hits;
			erhai_cache_hit_bytes += length;
			return true;
		}
		++erhai_cache_misses;
	}
#endif
	if (!erhai_transport_read(volume, offset, length, erhai_transport_buffer)) {
		return false;
	}
	for (uint32_t at = 0; at < length; at += VR_BLOCK_BYTES) {
		const uint8_t *hash = erhai_block_hash(volume, (offset + at) / VR_BLOCK_BYTES);
		const size_t size = MIN(length - at, VR_BLOCK_BYTES);
		uint8_t digest[erhai_hash_bytes];
		if (!hash) {
			return false;
		}
		if (!erhai_storage_hash(erhai_transport_buffer + at, size, digest) ||
		    memcmp(digest, hash, sizeof(digest))) {
			return erhai_read_failed("USB disk block hash", offset + at);
		}
	}
#if ERHAI_VERIFIED_CACHE
	if (cacheable) {
		erhai_cache_copy(volume, offset, length, true);
	}
#endif
	return true;
}

/* Every transport request costs a full USB round trip, while the filesystem
 * asks for 16 KiB at a time. Serve those page-ins from one verified window so
 * a sequential run of I/Os needs one request per window instead of one per
 * page. Authentication is unchanged: every 4 KiB block in the window is
 * checked against its pinned hash before any byte is handed out.
 *
 * A scattered fault pattern would make a large window read 16x more than it
 * uses, so the window only grows to 256 KiB while requests continue exactly
 * where the previous window ended. Any jump falls back to 64 KiB (4x at
 * worst, still one round trip). */
#if ERHAI_READAHEAD
#define ERHAI_WINDOW_BYTES (256u * 1024u)
#define ERHAI_WINDOW_MIN_BYTES (64u * 1024u)
static unsigned char erhai_window_buffer[ERHAI_WINDOW_BYTES] __attribute__((unused));
static uint64_t erhai_window_start __attribute__((unused));
static uint64_t erhai_window_end __attribute__((unused));
static uint32_t erhai_window_volume __attribute__((unused)) = ~0u;

static bool erhai_window_holds(uint32_t volume, uint64_t offset, uint32_t length)
{
	return erhai_window_volume == volume && offset >= erhai_window_start &&
	    offset + length <= erhai_window_end;
}

static bool erhai_fill_window(uint32_t volume, uint64_t start, uint32_t want)
{
	const uint64_t size = MIN((uint64_t)want, erhai_disk_bytes[volume] - start);
	if (!size) {
		return false;
	}
	if (!erhai_transport_read(volume, start, (uint32_t)size, erhai_window_buffer)) {
		erhai_window_volume = ~0u;
		return false;
	}
	for (uint32_t at = 0; at < size; at += VR_BLOCK_BYTES) {
		const uint8_t *hash = erhai_block_hash(volume, (start + at) / VR_BLOCK_BYTES);
		const uint64_t bytes = MIN((uint64_t)(size - at), (uint64_t)VR_BLOCK_BYTES);
		uint8_t digest[erhai_hash_bytes];
		if (!hash || !erhai_storage_hash(erhai_window_buffer + at, (size_t)bytes, digest) ||
		    memcmp(digest, hash, sizeof(digest))) {
			erhai_window_volume = ~0u;
			return false;
		}
	}
	erhai_window_volume = volume;
	erhai_window_start = start;
	erhai_window_end = start + size;
	return true;
}
#endif /* ERHAI_READAHEAD */
#endif

/* ---- read-only block device ---------------------------------------------- */
#if ERHAI_WITH_STORAGE

class ErhaiMuxBlock : public IOBlockStorageDevice {
	OSDeclareDefaultStructors(ErhaiMuxBlock)
	uint32_t volume = 0;
	uint64_t bytes = 0;
	char label[32] = {};
public:
	void configure(uint32_t index, const char *name, uint64_t size);
	bool start(IOService *provider) override;
	IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
	IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
	UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
	char *getVendorString() override { return const_cast<char *>("Erhai"); }
	char *getProductString() override { return label; }
	char *getRevisionString() override { return const_cast<char *>("1.0"); }
	char *getAdditionalDeviceInfoString() override { return const_cast<char *>("iPadOS volume over USBMux, read-only"); }
	IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
	IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
	IOReturn reportMaxValidBlock(UInt64 *value) override { *value = bytes / 512 - 1; return kIOReturnSuccess; }
	IOReturn reportMediaState(bool *present, bool *changed) override
	{
		*present = __atomic_load_n(&erhai_transport_ready, __ATOMIC_ACQUIRE) != 0;
		if (changed) { *changed = false; }
		return kIOReturnSuccess;
	}
	IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
	IOReturn reportWriteProtection(bool *value) override { *value = true; return kIOReturnSuccess; }
	IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
	IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
	IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
	IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
	IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
	                          IOStorageCompletion *) override;
};

OSDefineMetaClassAndStructors(ErhaiMuxBlock, IOBlockStorageDevice)

static ErhaiMuxBlock *erhai_disks[2];
#if ERHAI_USB_ROOT
static const char *const erhai_root_uuids[] = {
	"04769D26-2CA0-4825-B443-E2E5F88300F2", "8846FE78-2910-4297-AF97-D59BB6F8FC16",
};
extern "C" const uint8_t erhai_os_manifest_sha384[48];
extern "C" const uint8_t erhai_systemos_manifest_sha384[48];
#endif

static const char *erhai_media_property(IOService *entry, const char *key)
{
	const OSString *value = OSDynamicCast(OSString, entry->getProperty(key));
	return value ? value->getCStringNoCopy() : "";
}

static int erhai_media_volume(IOService *entry)
{
	IORegistryIterator *parents = IORegistryIterator::iterateOver(
	    entry, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
	if (!parents) {
		return -1;
	}
	int volume = -1;
	while (IORegistryEntry *parent = parents->getNextObject()) {
		for (unsigned i = 0; i < 2; ++i) {
			if (parent == __atomic_load_n(&erhai_disks[i], __ATOMIC_ACQUIRE)) {
				volume = (int)i;
				break;
			}
		}
		if (volume >= 0) {
			break;
		}
	}
	parents->release();
	return volume;
}

static bool erhai_media_scan(int volume, const char *uuid, char *out, size_t capacity, dev_t *device = NULL)
{
	OSDictionary *matching = IOService::serviceMatching("IOMedia");
	if (!matching) {
		return false;
	}
	OSIterator *media = IOService::getMatchingServices(matching);
	matching->release();
	if (!media) {
		return false;
	}
	size_t used = 0;
	bool found = false;
	while (auto *entry = OSDynamicCast(IOService, media->getNextObject())) {
		const char *name = erhai_media_property(entry, "BSD Name");
		const char *actual = erhai_media_property(entry, "UUID");
		const int owner = erhai_media_volume(entry);
		if (uuid) {
			if (owner == volume && *name && !strcasecmp(actual, uuid)) {
				if (found) {
					found = false;
					break;
				}
#if ERHAI_USB_ROOT
				if (device) {
					const OSNumber *major = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
					const OSNumber *minor = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
					if (!major || !minor || major->unsigned64BitValue() > 255 || minor->unsigned64BitValue() > 0xffffff) {
						continue;
					}
					const dev_t candidate = makedev(major->unsigned32BitValue(), minor->unsigned32BitValue());
					if (*device != NODEV && *device != candidate) {
						continue;
					}
					*device = candidate;
				}
#else
				(void)device;
#endif
				const int length = snprintf(out, capacity, "/dev/%s", name);
				found = length > 0 && (size_t)length < capacity;
			}
		} else {
			const int length = snprintf(out + used, capacity - used, "%d %s %s UUID=%s content=%s hint=%s\n",
			    owner, entry->getMetaClass()->getClassName(), name, actual,
			    erhai_media_property(entry, "Content"), erhai_media_property(entry, "Content Hint"));
			if (length < 0 || (size_t)length >= capacity - used) {
				break;
			}
			used += (size_t)length;
		}
	}
	media->release();
	return found;
}

extern "C" bool erhai_mux_media_path(unsigned volume, const char *uuid, char *path, size_t capacity)
{
	unsigned attempts = 300;
#if ERHAI_SYSTEMOS_RAM
	if (volume == 1) attempts = 2400;
#endif
	for (unsigned attempt = 0; attempt < attempts; ++attempt) {
		if (erhai_media_scan((int)volume, uuid, path, capacity)) {
			return true;
		}
		IOSleep(100);
	}
	return false;
}

#if ERHAI_USB_ROOT
extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
	if (!uuid || !device || !path || !size || !__atomic_load_n(&erhai_transport_ready, __ATOMIC_ACQUIRE)) {
		return 0;
	}
	for (unsigned volume = 0; volume < 2; ++volume) {
		if (!strcasecmp(uuid, erhai_root_uuids[volume])) {
			*device = NODEV;
			return erhai_media_scan(volume, uuid, path, size, device);
		}
	}
	return 0;
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
	if (device == NODEV || !__atomic_load_n(&erhai_transport_ready, __ATOMIC_ACQUIRE)) {
		return 0;
	}
	for (unsigned volume = 0; volume < 2; ++volume) {
		char path[64];
		if ((!uuid || !strcasecmp(uuid, erhai_root_uuids[volume])) &&
		    erhai_media_scan(volume, erhai_root_uuids[volume], path, sizeof(path), &device)) {
			if (digest) {
				memcpy(digest, volume == 0 ? erhai_os_manifest_sha384 : erhai_systemos_manifest_sha384, 48);
			}
			return 1;
		}
	}
	return 0;
}

extern "C" int native_verified_block_root_authorized(dev_t device)
{
	return native_verified_block_manifest_sha384(device, NULL, NULL);
}

#if ERHAI_HID_EARLY
static void erhai_start_early_touch()
{
	const IOReturn result = erhai_start_hid_input(erhai_touch_device, true);
	erhai_touch_early_result = uint32_t(result);
	if (boot_progress_root) boot_progress_root("early touch HID start", nullptr, uint32_t(result));
#if ERHAI_TOUCH_AUTO
	if (result == kIOReturnSuccess) {
		thread_t worker = nullptr;
		__atomic_store_n(&erhai_touch_worker_stage, 1, __ATOMIC_RELAXED);
		const kern_return_t status = kernel_thread_start(erhai_touch_worker, nullptr, &worker);
		if (status == KERN_SUCCESS) {
			thread_deallocate(worker);
		} else {
			__atomic_store_n(&erhai_touch_worker_status, status, __ATOMIC_RELAXED);
			__atomic_store_n(&erhai_touch_worker_stage, 4, __ATOMIC_RELEASE);
		}
	} else {
		__atomic_store_n(&erhai_touch_worker_status, result, __ATOMIC_RELAXED);
		__atomic_store_n(&erhai_touch_worker_stage, 4, __ATOMIC_RELEASE);
	}
#endif
}
#endif

extern "C" bool erhai_wait_for_usb_root(void)
{
	char path[64];
	const bool ready = erhai_mux_media_path(1, erhai_root_uuids[1], path, sizeof(path)) &&
	    erhai_mux_media_path(0, erhai_root_uuids[0], path, sizeof(path));
#if ERHAI_HID_EARLY
	if (ready) erhai_start_early_touch();
#if !ERHAI_LOCAL_ROOT
	if (ready && erhai_boot_quiet_console) erhai_boot_quiet_console();
#endif
#endif
	return ready;
}
#endif

void ErhaiMuxBlock::configure(uint32_t index, const char *name, uint64_t size)
{
	volume = index;
	bytes = size;
	strlcpy(label, name, sizeof(label));
}

bool ErhaiMuxBlock::start(IOService *provider)
{
	if (!IOBlockStorageDevice::start(provider)) {
		return false;
	}
	registerService();
	IOLog("[mux-block] published %s (%llu bytes)\n", label, (unsigned long long)bytes);
	return true;
}

IOReturn ErhaiMuxBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                         IOStorageAttributes *, IOStorageCompletion *completion)
{
	IOReturn status = kIOReturnSuccess;
	UInt64 done = 0;
	UInt64 length;

	if (!buffer || !completion || !completion->action) {
		return kIOReturnBadArgument;
	}
	if (buffer->getDirection() != kIODirectionIn) {
		completion->action(completion->target, completion->parameter, kIOReturnNotWritable, 0);
		return kIOReturnSuccess;
	}
	if (!__atomic_load_n(&erhai_transport_ready, __ATOMIC_ACQUIRE)) {
		completion->action(completion->target, completion->parameter, kIOReturnNotReady, 0);
		return kIOReturnSuccess;
	}
	if (count == 0 || block >= bytes / 512 || count > bytes / 512 - block ||
	    buffer->getLength() < count * 512) {
		completion->action(completion->target, completion->parameter, kIOReturnBadArgument, 0);
		return kIOReturnSuccess;
	}
	length = count * 512;

	IOLockLock(erhai_transport_lock);
#if ERHAI_LOCAL_ROOT
	if (!__atomic_load_n(&erhai_transport_ready, __ATOMIC_ACQUIRE)) {
#else
	if (!erhai_transport) {
#endif
		IOLockUnlock(erhai_transport_lock);
		completion->action(completion->target, completion->parameter, kIOReturnNotReady, 0);
		return kIOReturnSuccess;
	}
	while (done < length) {
		const UInt64 offset = block * 512 + done;
		UInt32 chunk = (UInt32)MIN(length - done, ERHAI_MAX_BLOCK);
		const uint8_t *source = erhai_transport_buffer;
		UInt32 skip = 0;
		bool valid;
		erhai_read_error = NULL;
#if ERHAI_VERIFY_STORAGE
		const UInt64 base = offset & ~(UInt64)(VR_BLOCK_BYTES - 1);
		UInt64 end = MIN((offset + chunk + VR_BLOCK_BYTES - 1) &
		    ~(UInt64)(VR_BLOCK_BYTES - 1), bytes);
		/* The transport rejects a request larger than ERHAI_MAX_BLOCK, and the
		 * aligned range can exceed it by up to one block. Clamp before any
		 * read: a rejected request would close the transport and drop the
		 * volume for the rest of the boot. */
		if (end - base > ERHAI_MAX_BLOCK) {
			end = base + ERHAI_MAX_BLOCK;
		}
		chunk = (UInt32)MIN((UInt64)chunk, end - offset);
#if ERHAI_READAHEAD
#if ERHAI_OS_WORKING_SET
		if (volume == 0 && os_resident) {
			valid = erhai_read_verified(volume, base, (UInt32)(end - base));
			skip = (UInt32)(offset - base);
		} else
#endif
		if (erhai_window_holds(volume, offset, chunk)) {
			source = erhai_window_buffer;
			skip = (UInt32)(offset - erhai_window_start);
			valid = true;
		} else {
			const bool sequential = erhai_window_volume == volume &&
			    offset >= erhai_window_end &&
			    offset - erhai_window_end <= VR_BLOCK_BYTES;
			UInt32 want = sequential ? ERHAI_WINDOW_BYTES : ERHAI_WINDOW_MIN_BYTES;
			want = (UInt32)MIN((UInt64)want, erhai_disk_bytes[volume] - base);
			/* Only pre-read a window that actually covers this request; the
			 * fallback below must not repeat an I/O the window already did. */
			if (base + want >= offset + chunk &&
			    erhai_fill_window(volume, base, want) &&
			    erhai_window_holds(volume, offset, chunk)) {
				source = erhai_window_buffer;
				skip = (UInt32)(offset - erhai_window_start);
				valid = true;
			} else {
				valid = erhai_read_verified(volume, base, (UInt32)(end - base));
				if (valid) {
					skip = (UInt32)(offset - base);
				}
			}
		}
#else
		valid = erhai_read_verified(volume, base, (UInt32)(end - base));
		if (valid) {
			skip = (UInt32)(offset - base);
		}
#endif
#else
		valid = erhai_transport_read(volume, offset, chunk, erhai_transport_buffer);
#endif
		if (!valid) {
			const char *reason = erhai_read_error ? erhai_read_error : "USB read failure";
			const uint64_t failed_offset = erhai_read_error ? erhai_read_error_offset : offset;
			snprintf(erhai_last_read_failure, sizeof(erhai_last_read_failure),
			    "%s vol=%u offset=0x%llx request=0x%llx length=%u errno=%d",
			    reason, volume, (unsigned long long)failed_offset, (unsigned long long)offset, chunk,
			    erhai_read_error ? erhai_read_errno : 0);
			IOLog("[mux-block] %s\n", erhai_last_read_failure);
			if (boot_progress_root) {
				boot_progress_root(reason, label, failed_offset);
			}
			if (erhai_transport) {
				sock_close(erhai_transport);
				erhai_transport = NULL;
				__atomic_store_n(&erhai_transport_ready, 0, __ATOMIC_RELEASE);
			}
			status = kIOReturnIOError;
			break;
		}
		if (buffer->writeBytes(done, source + skip, chunk) != chunk) {
			status = kIOReturnVMError;
			break;
		}
		done += chunk;
	}
	IOLockUnlock(erhai_transport_lock);

	if (status != kIOReturnSuccess) {
		IOLog("[mux-block] %s read failed at %llu\n", label, (unsigned long long)(block * 512 + done));
	}
	completion->action(completion->target, completion->parameter, status, done);
	return kIOReturnSuccess;
}

#endif /* ERHAI_WITH_STORAGE */

/* ---- probe listener ------------------------------------------------------ */

static void erhai_storage_abi(uint64_t report[8])
{
	const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
	const OSMetaClass *block = name ? OSMetaClass::getMetaClassWithName(name) : NULL;
	if (name) {
		name->release();
	}
	const OSMetaClass *parent = block ? block->getSuperClass() : NULL;
	report[0] = (uintptr_t)block;
	report[1] = (uintptr_t)parent;
	report[2] = block ? block->getClassSize() : 0;
	report[3] = parent ? parent->getClassSize() : 0;
	asm volatile("mrs %0, tcr_el1" : "=r"(report[4]));
	asm volatile("mrs %0, sctlr_el1" : "=r"(report[5]));
	asm volatile("mrs %0, ttbr0_el1" : "=r"(report[6]));
	asm volatile("mrs %0, ttbr1_el1" : "=r"(report[7]));
}

#if ERHAI_WITH_STORAGE
static bool erhai_adopt_backend(socket_t conn)
{
	unsigned mode = 0;
	if (!PE_parse_boot_argn("erhai-block", &mode, sizeof(mode)) || mode != 1 || !erhai_transport_lock) {
		return false;
	}
	IOLockLock(erhai_transport_lock);
	bool accepted = false;
	if (!erhai_transport && erhai_send_all(conn, "EBOK", 4)) {
		erhai_transport = conn;
		erhai_last_read_failure[0] = '\0';
#if ERHAI_VERIFIED_CACHE
		/* Check each incoming backend's real disk headers before using cached data. */
		memset(erhai_cache_entries, 0, sizeof(erhai_cache_entries));
#endif
		accepted =
#if ERHAI_VERIFY_STORAGE
		    erhai_read_verified(0, 0, VR_BLOCK_BYTES) &&
		    memcmp(erhai_transport_buffer + 512, "EFI PART", 8) == 0 &&
#if ERHAI_SYSTEMOS_RAM
		    erhai_preload_systemos() &&
#endif
#if ERHAI_OS_WORKING_SET
		    erhai_preload_os_working_set() &&
#endif
		    erhai_read_verified(1, 0, VR_BLOCK_BYTES) &&
#else
		    erhai_transport_read(0, 0, 4096, erhai_transport_buffer) &&
		    memcmp(erhai_transport_buffer + 512, "EFI PART", 8) == 0 &&
		    erhai_transport_read(1, 0, 4096, erhai_transport_buffer) &&
#endif
		    memcmp(erhai_transport_buffer + 32, "NXSB", 4) == 0;
#if ERHAI_SYSTEMOS_RAM
		if (boot_progress_root) {
			boot_progress_root(accepted ? "SystemOS RAM first block verified" : "SystemOS RAM backend refused",
			    accepted ? NULL : erhai_read_error, accepted ? 0 : erhai_read_error_offset);
		}
#if ERHAI_QUIET_CONSOLE
		if (accepted && erhai_boot_quiet_console) {
			erhai_boot_quiet_console();
		}
#endif
#endif
		if (!accepted) {
#if ERHAI_OS_WORKING_SET
			erhai_release_os_working_set();
#endif
#if ERHAI_SYSTEMOS_RAM
			erhai_release_systemos();
#endif
			erhai_transport = NULL;
		}
		__atomic_store_n(&erhai_transport_ready, accepted ? 1 : 0, __ATOMIC_RELEASE);
#if ERHAI_VERIFIED_CACHE
		if (accepted && !erhai_cache_data) {
			erhai_cache_data = static_cast<uint8_t *>(IOMalloc(erhai_cache_sets * erhai_cache_ways * VR_BLOCK_BYTES));
		}
#endif
	}
	IOLockUnlock(erhai_transport_lock);
	return accepted;
}
#endif

static void erhai_serve_probe(socket_t conn, const char *probe)
{
	if (memcmp(probe, erhai_probe_request, sizeof(erhai_probe_request) - 1) != 0) {
		return;
	}
	if (!erhai_send_all(conn, erhai_probe_reply, sizeof(erhai_probe_reply) - 1)) {
		return;
	}
	IOLog("[mux-probe] handshake ok, awaiting block config\n");

	struct ErhaiConfig config;
	if (!erhai_recv_all(conn, &config, sizeof(config))) {
		return;
	}
	if (memcmp(config.magic, "ECFG", 4) != 0 ||
	    config.block_size < 4096 || config.block_size > ERHAI_MAX_BLOCK ||
	    (config.block_size & 4095u) != 0 ||
	    config.count == 0 || config.count > ERHAI_MAX_REQUESTS) {
		return;
	}
	IOLog("[mux-probe] serving %u requests of %u bytes from volume %u\n",
	      (unsigned)config.count, (unsigned)config.block_size, (unsigned)config.volume);

	for (uint32_t index = 0; index < config.count; ++index) {
		struct ErhaiBlockRequest request;
		struct ErhaiBlockHeader header;
		struct ErhaiVerify verify;

		memcpy(request.magic, "EBK1", 4);
		request.volume = config.volume;
		request.offset = (uint64_t)index * config.block_size;
		request.length = config.block_size;
		if (!erhai_send_all(conn, &request, sizeof(request))) {
			return;
		}
		if (!erhai_recv_all(conn, &header, sizeof(header))) {
			return;
		}
		if (memcmp(header.magic, "EBK1", 4) != 0 || header.status != 0 ||
		    header.length != config.block_size) {
			return;
		}
		if (!erhai_recv_all(conn, erhai_block_buffer, config.block_size)) {
			return;
		}
		memcpy(verify.magic, "EVFY", 4);
		verify.status = 0;
		verify.checksum = erhai_fnv1a(erhai_block_buffer, config.block_size);
		verify.length = config.block_size;
		memcpy(verify.first, erhai_block_buffer, 4);
		if (!erhai_send_all(conn, &verify, sizeof(verify))) {
			return;
		}
	}
}

static void erhai_probe_accept_loop(void)
{
#if ERHAI_WITH_STORAGE && !ERHAI_LOCAL_ROOT
	erhai_transport_lock = IOLockAlloc();
#endif
	socket_t listener = NULL;
	errno_t err = sock_socket(AF_INET, SOCK_STREAM, 0, NULL, NULL, &listener);
	if (err != 0) {
		IOLog("[mux-probe] sock_socket failed %d\n", (int)err);
		return;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ERHAI_PROBE_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (sock_bind(listener, (const struct sockaddr *)&addr) != 0) {
		IOLog("[mux-probe] sock_bind failed\n");
		sock_close(listener);
		return;
	}
	if (sock_listen(listener, 4) != 0) {
		IOLog("[mux-probe] sock_listen failed\n");
		sock_close(listener);
		return;
	}
	IOLog("[mux-probe] listening on port %d\n", ERHAI_PROBE_PORT);

	for (;;) {
		socket_t conn = NULL;
		err = sock_accept(listener, NULL, 0, 0, NULL, NULL, &conn);
		if (err != 0) {
			IOSleep(100);
			continue;
		}
		const struct timeval timeout = { 10, 0 };
		char probe[sizeof(erhai_probe_request) - 1];
		if (!sock_setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) &&
		    !sock_setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) &&
		    erhai_recv_all(conn, probe, sizeof(probe))) {
			if (memcmp(probe, "ERHAI-ABI?!", sizeof(probe)) == 0) {
				uint64_t report[8];
				erhai_storage_abi(report);
				erhai_send_all(conn, report, sizeof(report));
#if ERHAI_AUX_CHECK
			} else if (memcmp(probe, "ERHAI-RESULT", sizeof(probe)) == 0) {
				int result[] = { __atomic_load_n(&erhai_aux_status[0], __ATOMIC_ACQUIRE),
				    __atomic_load_n(&erhai_aux_status[1], __ATOMIC_ACQUIRE) };
				erhai_send_all(conn, result, sizeof(result));
#endif
#if ERHAI_WITH_STORAGE
#if ERHAI_LIVE_UI_PROCESSES && ERHAI_UI_THREADS
			} else if (memcmp(probe, "ERHAI-THRD?", sizeof(probe)) == 0) {
				char report[3072] = {};
				boot_process_snapshot();
				size_t used = 0;
				for (const auto &row : boot_process_rows) {
					if (row[0]) used += snprintf(report + used, sizeof(report) - used, "%.119s\n", row);
				}
				if (!used) strlcpy(report, "Waiting for the first thread snapshot\n", sizeof(report));
				erhai_send_all(conn, report, sizeof(report));
				if (!__atomic_exchange_n(&erhai_thread_capture_pending, 1, __ATOMIC_ACQ_REL)) {
					thread_t worker = NULL;
					if (kernel_thread_start(erhai_capture_ui_threads, NULL, &worker) == KERN_SUCCESS) {
						thread_deallocate(worker);
					} else {
						__atomic_store_n(&erhai_thread_capture_pending, 0, __ATOMIC_RELEASE);
					}
				}
#endif
#if ERHAI_VERIFY_STORAGE && ERHAI_VERIFIED_CACHE
			} else if (memcmp(probe, "ERHAI-CACHE", sizeof(probe)) == 0 && erhai_transport_lock) {
				uint64_t report[4];
				IOLockLock(erhai_transport_lock);
				report[0] = erhai_cache_data ? erhai_cache_sets * erhai_cache_ways * VR_BLOCK_BYTES : 0;
				report[1] = erhai_cache_hits;
				report[2] = erhai_cache_misses;
				report[3] = erhai_cache_hit_bytes;
				IOLockUnlock(erhai_transport_lock);
				erhai_send_all(conn, report, sizeof(report));
#endif
#if ERHAI_HOME_INPUT
			} else if (memcmp(probe, "ERHAI-HOME!", sizeof(probe)) == 0) {
				erhai_home_report(conn, true);
			} else if (memcmp(probe, "ERHAI-HOME?", sizeof(probe)) == 0) {
				erhai_home_report(conn, false);
#endif
#if ERHAI_TOUCH_PROBE && !ERHAI_TOUCH_AUTO
			} else if (memcmp(probe, "ERHAI-TOUCH?", sizeof(probe)) == 0) {
				erhai_touch_probe(conn, false);
			} else if (memcmp(probe, "ERHAI-TOUCH!", sizeof(probe)) == 0) {
				erhai_touch_probe(conn, true);
#if ERHAI_TOUCH_DMA
			} else if (memcmp(probe, "ERHAI-TOUCHD", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn);
			} else if (memcmp(probe, "ERHAI-TOUCHT", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true);
			} else if (memcmp(probe, "ERHAI-GPI?!", sizeof(probe)) == 0) {
				erhai_touch_gpi_info(conn);
			} else if (memcmp(probe, "ERHAI-SMMU?", sizeof(probe)) == 0) {
				erhai_touch_smmu_info(conn);
			} else if (memcmp(probe, "ERHAI-SIDS?", sizeof(probe)) == 0) {
				erhai_touch_smmu_routes(conn);
			} else if (memcmp(probe, "ERHAI-GPI!!", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true);
			} else if (memcmp(probe, "ERHAI-GPILB", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::Duplex);
			} else if (memcmp(probe, "ERHAI-GPITX", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::Tx);
			} else if (memcmp(probe, "ERHAI-GPIRX", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::Rx);
			} else if (memcmp(probe, "ERHAI-GPIQR", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::QueueRx);
			} else if (memcmp(probe, "ERHAI-GPICR", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::ConfiguredRx);
			} else if (memcmp(probe, "ERHAI-GPIDR", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::ConfiguredRx, ErhaiTouchRoutes::Dual);
			} else if (memcmp(probe, "ERHAI-GPIDC", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::None, ErhaiTouchRoutes::Dual);
			} else if (memcmp(probe, "ERHAI-GPIDI", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::None, ErhaiTouchRoutes::DualTrim);
			} else if (memcmp(probe, "ERHAI-GPILT", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::LongTx, ErhaiTouchRoutes::Dual);
			} else if (memcmp(probe, "ERHAI-GPIFR", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::None,
				                     ErhaiTouchRoutes::DualCapture);
#if ERHAI_HOME_INPUT
			} else if (memcmp(probe, "ERHAI-GPIHD", sizeof(probe)) == 0) {
				erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::None,
				                     ErhaiTouchRoutes::DualHid);
#endif
			} else if (memcmp(probe, "ERHAI-GPIFW", sizeof(probe)) == 0) {
				auto *image = static_cast<uint8_t *>(IOMalloc(erhaiNtFirmwareSize));
				if (image) {
					if (erhai_recv_all(conn, image, erhaiNtFirmwareSize)) {
						erhai_touch_dma_probe(conn, true, true, ErhaiGpiLoopback::None,
						                     ErhaiTouchRoutes::Dual, image);
					}
					IOFree(image, erhaiNtFirmwareSize);
				}
#endif
#endif
			} else if (memcmp(probe, "ERHAI-DISP?", sizeof(probe)) == 0) {
				char report[4096] = {};
				if (native_legacy_clients_dump) {
					native_legacy_clients_dump(report, sizeof(report));
				} else {
					strlcpy(report, "Display client diagnostics unavailable\n", sizeof(report));
				}
				erhai_send_all(conn, report, sizeof(report));
			} else if (memcmp(probe, "ERHAI-BOOT?", sizeof(probe)) == 0 && erhai_boot_progress_snapshot) {
				char report[2048] = {};
				erhai_boot_progress_snapshot(report, sizeof(report));
				const size_t used = strlen(report);
				if (&native_legacy_stage && &native_legacy_client_calls &&
				    &native_legacy_frame_count && &native_legacy_client_status) {
					snprintf(report + used, sizeof(report) - used,
					    "DISPLAY stage=%u clients=%u status=0x%08x frames=%u\n",
					    native_legacy_stage, native_legacy_client_calls,
					    (unsigned)native_legacy_client_status, native_legacy_frame_count);
				}
#if ERHAI_KEYSTORE_EVENTS
				const size_t end = strlen(report);
				snprintf(report + end, sizeof(report) - end, "KEYSTORE registered=%u opens=%u calls=%u\n",
				    __atomic_load_n(&erhai_keystore_state, __ATOMIC_RELAXED),
				    __atomic_load_n(&erhai_keystore_opens, __ATOMIC_RELAXED),
				    __atomic_load_n(&erhai_keystore_calls, __ATOMIC_RELAXED));
#endif
#if ERHAI_LIVE_UI_PROCESSES
				erhai_append_ui_status(report, sizeof(report));
#endif
#if ERHAI_TOUCH_AUTO
				const unsigned stage = __atomic_load_n(&erhai_touch_worker_stage, __ATOMIC_ACQUIRE);
				const size_t touchEnd = strlen(report);
				snprintf(report + touchEnd, sizeof(report) - touchEnd,
				    "TOUCH stage=%u result=%d reports=%u dispatch=%u errors=%u\n", stage,
				    __atomic_load_n(&erhai_touch_worker_status, __ATOMIC_RELAXED),
				    native_touch_hid_reports, native_touch_edge_stats[5],
				    __atomic_load_n(&erhai_touch_hid_errors, __ATOMIC_RELAXED));
#endif
				erhai_send_all(conn, report, sizeof(report));
			} else if (memcmp(probe, "ERHAI-FAIL?", sizeof(probe)) == 0 && erhai_transport_lock) {
				char report[sizeof(erhai_last_read_failure)];
				IOLockLock(erhai_transport_lock);
				memcpy(report, erhai_last_read_failure, sizeof(report));
				IOLockUnlock(erhai_transport_lock);
				erhai_send_all(conn, report, sizeof(report));
			} else if (memcmp(probe, "ERHAI-MEDIA?", sizeof(probe)) == 0) {
				char inventory[3072] = {};
				erhai_media_scan(-1, NULL, inventory, sizeof(inventory));
				erhai_send_all(conn, inventory, sizeof(inventory));
			} else if (memcmp(probe, "EBL2", 4) == 0) {
				if (erhai_adopt_backend(conn)) {
					continue;
				}
#endif
			} else {
				erhai_serve_probe(conn, probe);
			}
		}
		sock_close(conn);
	}
}

static void erhai_probe_thread(void *, wait_result_t)
{
	IOSleep(6000);
	erhai_probe_accept_loop();
	thread_terminate(current_thread());
}

extern "C" void erhai_mux_probe_start(void)
{
	static uint32_t started;
	if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) {
		return;
	}
	thread_t thread = NULL;
	if (kernel_thread_start(erhai_probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		IOLog("[mux-probe] kernel_thread_start failed\n");
	}
}

#if ERHAI_WITH_STORAGE
extern "C" void erhai_mux_block_publish(IOService *provider)
{
	static bool attempted = false;
	if (attempted || !provider || !__atomic_load_n(&erhai_transport_ready, __ATOMIC_ACQUIRE)) {
		return;
	}
	uint64_t report[8];
	erhai_storage_abi(report);
	if (report[0] != ERHAI_ORIGINAL_STORAGE_META || report[1] != (uintptr_t)&IOService::gMetaClass ||
	    report[2] != 0x90 || report[3] != 0x88 || (report[4] >> 14 & 3) != 0 ||
	    (report[4] >> 30 & 3) != 2 || !(report[5] & 1) || !report[6] || !report[7]) {
		return;
	}

	attempted = true;
#if ERHAI_USB_ROOT
	const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
	if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {
		IOLog("[mux-block] APFS kext failed to load: %#x\n", loaded);
		return;
	}
#endif
	void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	if (!handle) {
		return;
	}
	erhai_storage_static_init();
	if (OSMetaClass::postModLoad(handle)) {
		return;
	}

#if ERHAI_KEYSTORE_EVENTS
	/* Publish the RAM-only state service before starting the original launchd. */
	const char *keystore_status = "KeyStore publication failed";
	if (auto *service = native_keyevents_allocate()) {
		if (service->init() && service->attach(provider)) {
			if (service->start(provider)) {
				keystore_status = "KeyStore events ready";
				__atomic_store_n(&erhai_keystore_state, 1, __ATOMIC_RELAXED);
			} else {
				service->detach(provider);
			}
		}
		service->release();
	}
	if (boot_progress_root) boot_progress_root(keystore_status, NULL, 0);
#endif

	const struct { unsigned volume; const char *name; uint64_t bytes; } disks[] = {
		{ 1, "Erhai SystemOS", ERHAI_SYSTEMOS_BYTES },
		{ 0, "Erhai OS", ERHAI_OS_BYTES },
	};
	for (const auto &disk : disks) {
		ErhaiMuxBlock *device = OSTypeAlloc(ErhaiMuxBlock);
		if (!device || !device->init(static_cast<OSDictionary *>(nullptr))) {
			if (device) {
				device->release();
			}
			return;
		}
		device->configure(disk.volume, disk.name, disk.bytes);
		if (!device->attach(provider)) {
			device->release();
			return;
		}
		if (!device->start(provider)) {
			device->detach(provider);
			device->release();
			return;
		}
		__atomic_store_n(&erhai_disks[disk.volume], device, __ATOMIC_RELEASE);
	}
}

#if ERHAI_LOCAL_ROOT
extern "C" int erhai_open_local_volumes(void);

extern "C" bool erhai_start_local_root(void)
{
	const int status = erhai_open_local_volumes();
	if (status) {
		panic("local ext4 root open failed: %d", status);
	}
	erhai_transport_lock = IOLockAlloc();
	if (!erhai_transport_lock) return false;
	if (boot_progress_root) boot_progress_root("local iPadOS files open", NULL, 0);
	IOLockLock(erhai_transport_lock);
	const bool accepted = erhai_read_verified(0, 0, VR_BLOCK_BYTES) &&
	    memcmp(erhai_transport_buffer + 512, "EFI PART", 8) == 0 &&
	    erhai_read_verified(1, 0, VR_BLOCK_BYTES) &&
	    memcmp(erhai_transport_buffer + 32, "NXSB", 4) == 0;
	IOLockUnlock(erhai_transport_lock);
	if (!accepted) {
		panic("local root verification failed: %s", erhai_read_error ? erhai_read_error : "disk header");
	}
	if (boot_progress_root) boot_progress_root("local iPadOS first blocks verified", NULL, 0);
#if ERHAI_TOUCH_DMA
	if (boot_progress_root) boot_progress_root("TOUCH SCREEN CHECK READY", NULL, 0);
#endif
#if ERHAI_VERIFIED_CACHE
	erhai_cache_data = static_cast<uint8_t *>(IOMalloc(erhai_cache_sets * erhai_cache_ways * VR_BLOCK_BYTES));
#endif
	__atomic_store_n(&erhai_transport_ready, 1, __ATOMIC_RELEASE);
	erhai_mux_block_publish(IOService::getResourceService());
	if (!__atomic_load_n(&erhai_disks[0], __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&erhai_disks[1], __ATOMIC_ACQUIRE)) return false;
	const bool mounted = erhai_wait_for_usb_root();
	if (mounted && erhai_boot_quiet_console) erhai_boot_quiet_console();
	return mounted;
}
#endif

#endif /* ERHAI_WITH_STORAGE */

#if !ERHAI_WITH_STORAGE
extern "C" void erhai_mux_block_publish(IOService *) {}
#endif
