#include <stdint.h>

/* NativeRamBlock's optional persistence ABI. This profile has no UFS backend. */
unsigned native_ufs_store_mode(void) { return 0; }
unsigned native_ufs_store_gpt_check(void) { return 0xff; }
int native_ufs_store_sync(void) { return 0; }

int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why)
{
	(void)blocks;
	(void)first;
	if (why) {
		*why = "volatile Data profile";
	}
	return 0;
}

int native_ufs_store_read(uint64_t block, uint32_t count, void *data)
{
	(void)block;
	(void)count;
	(void)data;
	return 0;
}

int native_ufs_store_write(uint64_t block, uint32_t count, const void *data)
{
	(void)block;
	(void)count;
	(void)data;
	return 0;
}
