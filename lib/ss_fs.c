#include <stdlib.h>

#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include "ss_cache.h"
#include "ss_provision.h"
#include <onomondo/utils/ss_profile.h>
#include <onomondo/softsim/fs.h>
#include <onomondo/softsim/list.h>
#include <onomondo/softsim/utils.h>
#include <onomondo/softsim/log.h>
#include <onomondo/softsim/mem.h>

LOG_MODULE_DECLARE(softsim, CONFIG_SOFTSIM_LOG_LEVEL);

#define NVS_PARTITION        nvs_storage
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)

#define DIR_ID (1UL)

#define IMSI_PATH  "/3f00/7ff0/6f07"
#define ICCID_PATH "/3f00/2fe2"
#define A001_PATH  "/3f00/a001"
#define A004_PATH  "/3f00/a004"
#define SMSP_PATH  "/3f00/7ff0/6f42"

/* Binary (NVS) sizes — the uicc ss_profile.h defines sizes in hex-char counts;
 * IMSI_BIN etc. are the corresponding raw byte counts stored in NVS. */
#define IMSI_BIN_LEN  (IMSI_LEN  / 2)  /* 9  bytes  */
#define ICCID_BIN_LEN (ICCID_LEN / 2)  /* 10 bytes  */
#define KEY_BIN_LEN   (KEY_SIZE  / 2)  /* 16 bytes  */
#define A001_BIN_LEN  (A001_LEN  / 2)  /* 33 bytes  */
#define A004_BIN_LEN  (A004_LEN  / 2)  /* 114 bytes */

#ifndef SEEK_SET
#define SEEK_SET 0 /* set file offset to offset */
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1 /* set file offset to current plus offset */
#endif
#ifndef SEEK_END
#define SEEK_END 2 /* set file offset to EOF plus offset */
#endif

static struct nvs_fs fs;
static struct ss_list fs_cache;

static uint8_t fs_is_initialized = 0;
static uint8_t default_imsi[] = {0x08, 0x09, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10};

/**
 * @brief Internal function to read NVS data into cache
 *
 * @param entry Pointer to the cache entry to read data into
 *
 * This function reads the content of a cache entry from NVS and stores it in the
 * cache's buffer. If the buffer is already allocated, it reuses it if possible.
 * If not, it allocates a new buffer of the required size.
 *
 * It also handles writing dirty buffers back to NVS if necessary.
 * */
static void ss_read_nvs_to_cache(struct cache_entry *entry);

/* See in fs_port.h */
int ss_init_fs(void)
{
	if (fs_is_initialized) {
		return 0; /* Already initialized */
	}

	ss_list_init(&fs_cache);
	uint8_t *data = NULL;
	size_t len = 0;

	fs.flash_device = NVS_PARTITION_DEVICE;
	fs.sector_size =
		0x1000; /* Where to read this? :DT_PROP(NVS_PARTITION, erase_block_size); */
	fs.sector_count = FLASH_AREA_SIZE(nvs_storage) / fs.sector_size;
	fs.offset = NVS_PARTITION_OFFSET;

	int rc = nvs_mount(&fs);
	if (rc) {
		LOG_ERR("Failed to mount NVS");
		return -1;
	}

	rc = nvs_read(&fs, DIR_ID, NULL, 0);
	len = rc;

	/* Read DIR_ENTRY from NVS
	 * This is used to construct a linked list that
	 * serves as a cache and lookup table for the filesystem
	 */
	if (!data && rc) {
		data = SS_ALLOC_N(len * sizeof(uint8_t));
		rc = nvs_read(&fs, DIR_ID, data, len);
		__ASSERT_NO_MSG(rc == len);
	}

	ss_list_init(&fs_cache);
	generate_dir_table_from_blob(&fs_cache, data, len);

	if (ss_list_empty(&fs_cache)) {
		goto out;
	}

	fs_is_initialized++;

out:
	SS_FREE(data);
	return ss_list_empty(&fs_cache);
}

/* See in fs_port.h */
int ss_deinit_fs(void)
{
	/* TODO: check if DIR entry is still valid. If not recreate and write.
	 * Will NVS only commit if there are changes? If so, we can just recreate
	 * and let NVS do the compare.
	 */

	struct cache_entry *cursor, *pre_cursor;

	/* Free all memory allocated by cache and commit changes to NVS */
	SS_LIST_FOR_EACH_SAVE(&fs_cache, cursor, pre_cursor, struct cache_entry, list)
	{
		if (cursor->_b_dirty) {
			LOG_INF("SoftSIM stop - committing %s to NVS", cursor->name);
			nvs_write(&fs, cursor->key, cursor->buf, cursor->_l);
		}

		ss_list_remove(&cursor->list);

		if (cursor->buf) {
			SS_FREE(cursor->buf);
		}
		SS_FREE(cursor->name);
		SS_FREE(cursor);
	}

	fs_is_initialized = 0;

	return 0;
}

/* See in fs_port.h */
ss_FILE ss_fopen(char *path, char *mode)
{
	struct cache_entry *cursor = NULL;
	int rc = 0;

	cursor = f_cache_find_by_name(path, &fs_cache);
	if (!cursor) {
		return NULL;
	}

	/* Currently not used.
	 * Could potentially be used in the future to re-arrange order.
	 * Initial order is ordered by frequency already so not big optimizations can
	 * be achieved.
	 */
	if (cursor->_cache_hits < 0xFF) {
		cursor->_cache_hits++;
	}

	if (!cursor->_l) {
		rc = nvs_read(&fs, cursor->key, NULL, 0);
		if (rc < 0) {
			return NULL; /* TODO: This can not happen */
		} else {
			cursor->_l = rc;
		}
	}

	/* Reset internal read/write pointer */
	cursor->_p = 0;

	/* Guarantee buffer contains valid data */
	ss_read_nvs_to_cache(cursor);
	return (void *)cursor;
}

/* The POSIX default in onomondo-uicc/src/softsim/fs.c is not
 * compiled when CONFIG_COMPACT_STORAGE=ON, so we must provide it here. */
int ss_file_size(const char *path)
{
	struct cache_entry *entry = f_cache_find_by_name(path, &fs_cache);

	if (!entry) {
		return -1;
	}

	if (!entry->_l) {
		int rc = nvs_read(&fs, entry->key, NULL, 0);

		if (rc < 0) {
			return -1;
		}
		entry->_l = rc;
	}

	return (int)entry->_l;
}

/* See in fs_port.h */
size_t ss_fread(void *ptr, size_t size, size_t nmemb, ss_FILE fp)
{
	if (nmemb == 0 || size == 0) {
		return 0;
	}

	struct cache_entry *entry = (struct cache_entry *)fp;
	size_t max_element_to_return = (entry->_l - entry->_p) / size;
	size_t element_to_return = nmemb > max_element_to_return ? max_element_to_return : nmemb;

	/* Copy data from cache to user buffer */
	memcpy(ptr, entry->buf + entry->_p, element_to_return * size);
	/* Update internal read/write pointer */
	entry->_p += element_to_return * size;

	return element_to_return;
}

void ss_read_nvs_to_cache(struct cache_entry *entry)
{
	struct cache_entry *tmp;

	if (entry->buf) {
		return;
	}

	tmp = f_cache_find_buffer(entry, &fs_cache);

	uint8_t *buffer_to_use = NULL;
	size_t buffer_size = 0;

	if (tmp) {
		if (tmp->_b_dirty) {
			LOG_DBG("Cache entry %s is dirty, writing to NVS", tmp->name);
			nvs_write(&fs, tmp->key, tmp->buf, tmp->_l);
		}

		if (entry->_l > tmp->_b_size) {
			SS_FREE(tmp->buf);
		} else {
			buffer_size = tmp->_b_size;
			buffer_to_use = tmp->buf;
			memset(buffer_to_use, 0, buffer_size);
		}

		tmp->buf = NULL;
		tmp->_b_size = 0;
		tmp->_b_dirty = 0;
	}

	if (!buffer_to_use) {
		buffer_size = entry->_l;
		LOG_DBG("Allocating buffer of size %d", buffer_size);
		buffer_to_use = SS_ALLOC_N(buffer_size * sizeof(uint8_t));
	}

	if (!buffer_to_use) {
		LOG_ERR("Failed to allocate buffer of size %d", buffer_size);
		return;
	}

	int rc = nvs_read(&fs, entry->key, buffer_to_use, entry->_l);
	if (rc < 0) {
		LOG_ERR("NVS read failed: %d", rc);
		SS_FREE(buffer_to_use);
		return;
	}

	entry->buf = buffer_to_use;
	entry->_b_size = buffer_size;
	entry->_b_dirty = 0;
}

char *ss_fgets(char *str, int n, ss_FILE fp)
{
	struct cache_entry *entry = (struct cache_entry *)fp;

	if (!entry) {
		LOG_ERR("Invalid file pointer, ss_fgets failed");
		return NULL;
	}

	if (entry->_p >= entry->_l) {
		/* No more data to read */
		return NULL;
	}

	int idx = 0; /* Destination buffer index */

	while (entry->_p < entry->_l && idx < n - 1 && entry->buf[entry->_p] != '\0' &&
	       entry->buf[entry->_p] != '\n') {
		str[idx++] = entry->buf[entry->_p++];
	}

	str[idx] = '\0';
	return str;
}

int ss_fclose(ss_FILE fp)
{
	struct cache_entry *entry = (struct cache_entry *)fp;

	if (!entry) {
		LOG_ERR("Invalid file pointer, ss_fclose failed");
		return -1;
	}

	if (entry->_flags & FS_READ_ONLY) {
		goto out;
	}

	if (entry->_flags & FS_COMMIT_ON_CLOSE) {
		if (entry->_b_dirty) {
			nvs_write(&fs, entry->key, entry->buf, entry->_l);
		}
		entry->_b_dirty = 0;
	}

out:
	entry->_p = 0; /* TODO: Resetting internal read/write pointer not needed? */
	return 0;
}

int ss_fseek(ss_FILE fp, long offset, int whence)
{
	struct cache_entry *entry = (struct cache_entry *)fp;

	if (!entry) {
		LOG_ERR("Invalid file pointer, ss_fseek failed");
		return -1;
	}

	if (whence == SEEK_SET) {
		entry->_p = offset;
	} else if (whence == SEEK_CUR) {
		entry->_p += offset;
		if (entry->_p >= entry->_l) {
			entry->_p = entry->_l - 1;
		}
	} else if (whence == SEEK_END) {
		entry->_p = entry->_l - offset;
	}

	return 0;
}

long ss_ftell(ss_FILE fp)
{
	struct cache_entry *entry = (struct cache_entry *)fp;

	if (!entry) {
		return -1;
	}

	return entry->_p;
}

int ss_fputc(int c, ss_FILE fp)
{
	struct cache_entry *entry = (struct cache_entry *)fp;

	if (!entry) {
		return -1;
	}

	if (entry->_p >= entry->_b_size) {
		uint8_t *old_buffer = entry->buf;
		size_t old_size = entry->_b_size;
		entry->buf = SS_ALLOC_N(entry->_b_size + 20);

		if (!entry->buf) {
			entry->buf = old_buffer;
			return -1;
		}

		memcpy(entry->buf, old_buffer, old_size);
		entry->_b_size += 20;
	}

	entry->buf[entry->_p++] = (uint8_t)c;
	entry->_b_dirty = 1;
	entry->_l = entry->_l >= entry->_p ? entry->_l : entry->_p;

	return c;
}

int ss_access(const char *path, int amode)
{
	/* TODO: This is safe to omit for now. Internally SoftSIM will verify that a
	 * directory exists after creation. Easier to guarantee since it isn't a 'thing'
	 */

	return 0;
}

int ss_mkdir(const char *path, int mode)
{
	/* We don't care. This is a virtual filesystem, so directories
	 * are not really a thing. We just create files and directories are
	 * implicitly created.
	 */

	return 0;
}

int ss_rmdir(const char *path)
{
	/* TODO: Remove all entries with directory match */
	return 0;
}

int ss_remove(const char *path)
{
	struct cache_entry *entry = f_cache_find_by_name(path, &fs_cache);

	if (!entry) {
		return -1;
	}

	ss_list_remove(&entry->list);
	nvs_delete(&fs, entry->key);

	if (entry->buf) {
		SS_FREE(entry->buf);
	}

	SS_FREE(entry->name);
	SS_FREE(entry);

	return 0;
}

size_t ss_fwrite(const void *ptr, size_t size, size_t count, ss_FILE fp)
{
	struct cache_entry *entry = (struct cache_entry *)fp;

	if (!entry) {
		return -1;
	}

	const size_t requiredBufferSize = entry->_p + size * count;

	if (requiredBufferSize > entry->_b_size) {
		uint8_t *oldBuffer = entry->buf;
		const size_t oldSize = entry->_b_size;

		entry->buf = SS_ALLOC_N(requiredBufferSize);

		if (!entry->buf) {
			entry->buf = oldBuffer;
			return -1;
		} else {
			entry->_b_size = requiredBufferSize;
		}

		memcpy(entry->buf, oldBuffer, oldSize);
		SS_FREE(oldBuffer);
	}

	const size_t buffer_left = entry->_b_size - entry->_p;
	const size_t elements_to_copy = buffer_left > size * count ? count : buffer_left / size;

	const uint8_t content_is_different =
		memcmp(entry->buf + entry->_p, ptr, size * elements_to_copy);

	if (content_is_different) {
		memcpy(entry->buf + entry->_p, ptr, size * elements_to_copy);
		entry->_b_dirty = 1;
	}
	entry->_p += size * elements_to_copy;

	return elements_to_copy;
}

/**
 * @brief Checks if the SoftSIM is provisioned
 *
 * @return 1 if provisioned, 0 if not provisioned
 */
int port_check_provisioned(void)
{
	int ret;
	uint8_t buffer[IMSI_BIN_LEN] = {0};
	struct cache_entry *entry =
		(struct cache_entry *)f_cache_find_by_name(IMSI_PATH, &fs_cache);

	ret = nvs_read(&fs, entry->key, buffer, IMSI_BIN_LEN);
	if (ret < 0) {
		return 0;
	}

	if (memcmp(buffer, default_imsi, IMSI_BIN_LEN) == 0) {
		return 0;
	}

	return 1;
}

/* Decode a hex-character string to raw bytes.
 * hex: input hex-char string (2 chars per byte)
 * hex_len: number of hex characters (= 2 * number of output bytes)
 * out: output buffer (must have at least hex_len/2 bytes) */
static void hex_str_to_bytes(const uint8_t *hex, size_t hex_len, uint8_t *out)
{
	for (size_t i = 0; i < hex_len / 2; i++) {
		uint8_t hi = hex[i * 2], lo = hex[i * 2 + 1];

		out[i] = ((hi % 32 + 9) % 25 * 16) + ((lo % 32 + 9) % 25);
	}
}

/**
 * @brief Provisions SoftSIM with the given profile
 *
 * Decodes hex-char fields from the uicc ss_profile struct into the binary
 * format expected by the NVS filesystem. A001 and A004 are stored in
 * pseudo-key format so that ss_crypto can route calls to the nRF KMU:
 *   A001: { KI_TAG, zeros[15], OPC[16], 0x00 }
 *   A004: { header[6], KIC_TAG, zeros[15], KID_TAG, zeros[15], 0xFF... }
 *
 * @param profile Decoded uicc profile (from ss_profile_from_string)
 * @return 0 on success, -1 on failure
 */
int port_provision(struct ss_profile *profile)
{
	int rc = ss_init_fs();

	if (rc) {
		LOG_ERR("Failed to init FS");
	}

	/* IMSI: hex chars → binary */
	uint8_t imsi_bin[IMSI_BIN_LEN];

	hex_str_to_bytes(profile->_3F00_7ff0_6f07, IMSI_LEN, imsi_bin);

	/* ICCID: hex chars → binary */
	uint8_t iccid_bin[ICCID_BIN_LEN];

	hex_str_to_bytes(profile->_3F00_2FE2, ICCID_LEN, iccid_bin);

	/* A001: construct KMU pseudo-key format { KI_TAG, zeros[15], OPC[16], 0x00 }.
	 * OPC is at _3F00_A001[KEY_SIZE .. KEY_SIZE*2-1] (hex chars). */
	uint8_t a001_bin[A001_BIN_LEN];

	memset(a001_bin, 0, sizeof(a001_bin));
	a001_bin[0] = KI_TAG; /* routing indicator: tells ss_crypto to use KMU KI slot */
	hex_str_to_bytes(&profile->_3F00_A001[KEY_SIZE], KEY_SIZE, &a001_bin[KEY_BIN_LEN]);

	/* A004: construct KMU pseudo-key format
	 * { header[6], KIC_TAG, zeros[15], KID_TAG, zeros[15], 0xFF... } */
	static const uint8_t a004_hdr[] = {0xb0, 0x00, 0x11, 0x06, 0x01, 0x01};
	uint8_t a004_bin[A004_BIN_LEN];

	memcpy(a004_bin, a004_hdr, sizeof(a004_hdr));
	a004_bin[6] = KIC_TAG; /* routing indicator: KMU KIC slot */
	memset(&a004_bin[7], 0, KEY_BIN_LEN - 1);
	a004_bin[6 + KEY_BIN_LEN] = KID_TAG; /* routing indicator: KMU KID slot */
	memset(&a004_bin[7 + KEY_BIN_LEN], 0, KEY_BIN_LEN - 1);
	memset(&a004_bin[6 + 2 * KEY_BIN_LEN], 0xFF,
	       sizeof(a004_bin) - 6 - 2 * KEY_BIN_LEN);

	struct cache_entry *entry;

	LOG_INF("Provisioning SoftSIM 1/5");
	entry = (struct cache_entry *)f_cache_find_by_name(IMSI_PATH, &fs_cache);
	if (nvs_write(&fs, entry->key, imsi_bin, sizeof(imsi_bin)) < 0) {
		goto out_err;
	}
	entry->_flags = 0;

	LOG_INF("Provisioning SoftSIM 2/5");
	entry = (struct cache_entry *)f_cache_find_by_name(ICCID_PATH, &fs_cache);
	if (nvs_write(&fs, entry->key, iccid_bin, sizeof(iccid_bin)) < 0) {
		goto out_err;
	}
	entry->_flags = 0;

	LOG_INF("Provisioning SoftSIM 3/5");
	entry = (struct cache_entry *)f_cache_find_by_name(A001_PATH, &fs_cache);
	if (nvs_write(&fs, entry->key, a001_bin, sizeof(a001_bin)) < 0) {
		goto out_err;
	}
	entry->_flags = 0;

	LOG_INF("Provisioning SoftSIM 4/5");
	entry = (struct cache_entry *)f_cache_find_by_name(A004_PATH, &fs_cache);
	if (nvs_write(&fs, entry->key, a004_bin, sizeof(a004_bin)) < 0) {
		goto out_err;
	}
	entry->_flags = 0;

	/* Provision SMSP if provided in profile (non-zero) */
	uint8_t smsp_bin[SMSP_RECORD_SIZE];
	uint8_t smsp_zeros[SMSP_RECORD_SIZE];

	hex_str_to_bytes(profile->SMSP, SMSP_RECORD_SIZE * 2, smsp_bin);
	memset(smsp_zeros, 0, sizeof(smsp_zeros));

	if (memcmp(smsp_bin, smsp_zeros, sizeof(smsp_bin)) != 0) {
		LOG_INF("Provisioning SoftSIM 5/5 (SMSP)");
		entry = (struct cache_entry *)f_cache_find_by_name(SMSP_PATH, &fs_cache);
		if (entry) {
			/* Two-record SMSP file: write first record, pad second with 0xFF */
			uint8_t smsp_full[SMSP_RECORD_SIZE * 2];

			memcpy(smsp_full, smsp_bin, SMSP_RECORD_SIZE);
			memset(smsp_full + SMSP_RECORD_SIZE, 0xFF, SMSP_RECORD_SIZE);
			if (nvs_write(&fs, entry->key, smsp_full, sizeof(smsp_full)) < 0) {
				goto out_err;
			}
			entry->_flags = 0;
		}
	} else {
		LOG_INF("Provisioning SoftSIM 5/5 (SMSP not provided, skipping)");
	}

	LOG_INF("SoftSIM provisioned");
	return 0;

out_err:
	LOG_ERR("SoftSIM provisioning failed");
	return -1;
}
