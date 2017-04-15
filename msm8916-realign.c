/*
* msm8916-realign - small tool for aligning beginning of partitions to eMMC erase unit size boundaries
*
* Written by Vasiliy Sychev (https://github.com/vasiliy-sychev)
*
* Documents and files used during development:
* Wikipedia article (https://en.wikipedia.org/wiki/GUID_Partition_Table)
* UEFI Specification v2.3.1 (http://www.uefi.org/specifications)
* patch0.xml (from original firmware package for Xiaomi Redmi 2)
* gpt_main0.bin and gpt_backup0.bin (from original firmware package for Xiaomi Redmi 2)
*
* This software comes without any warranties, use it at your own risk!
*
* Tested on Xiaomi Redmi 2 (wt88047) - device boots, and works great after modification!
*/

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Without this, size of structures may be greater and internal test fails. */
/* #pragma pack(4) /* Uncomment this line, if Structure alignment not set in project settings */

/* On Redmi 2 (and maybe other MSM8916-based devices) first 64 megabytes reserved for updates (?) */
#define FIRST_USABLE_SECTOR  131072

/* eMMC devices inside a Qualcomm MSM8916-based machines usually have 512 byte sectors */
#define EMMC_SECTOR_SIZE     512

/* Size of useful part of GPT header */
#define GPT_HEADER_SIZE      92

/* For this moment, only 128 byte-sized entries are supported */
#define GPT_PART_ENTRY_SIZE  128

/* Other important values */
#define GUID_SIZE            16
#define PART_NAME_SIZE       36

/* File sizes */
/* gpt_main0.bin */
#define MAIN_SIZE_BYTES      17408
#define MAIN_SIZE_SECTORS    34

/* gpt_backup0.bin */
#define BACKUP_SIZE_BYTES    16896
#define BACKUP_SIZE_SECTORS  33

/* Functions from crc32.c */
unsigned int chksum_crc32 (unsigned char *block, unsigned int length);
void chksum_crc32gentab();

struct gpt_part_entry
{
	unsigned char part_type_guid[GUID_SIZE];
	unsigned char unique_guid[GUID_SIZE];
	unsigned __int64 starting_lba;
	unsigned __int64 ending_lba;
	unsigned __int64 attributes;
	wchar_t part_name[PART_NAME_SIZE];
};

struct gpt_header
{
	char signature[8];
	unsigned int revision;
	unsigned int header_size;
	unsigned int header_crc32;
	unsigned int reserved;
	unsigned __int64 my_lba;
	unsigned __int64 alternate_lba;
	unsigned __int64 first_usable_lba;
	unsigned __int64 last_usable_lba;
	unsigned char disk_guid[GUID_SIZE];
	unsigned __int64 partition_entry_lba;
	unsigned int num_of_partition_entries;
	unsigned int size_of_partition_entry;
	unsigned int part_entry_array_crc32;
};

struct mem_block
{
	void *data;
	DWORD length;
};

int load_file(struct mem_block *mb, wchar_t *file_name)
{
	HANDLE file;
	DWORD file_size;
	DWORD bytes_returned;

	wprintf(L"Loading data from file... ");

	file = CreateFile(file_name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);

	if(file == INVALID_HANDLE_VALUE)
	{
		wprintf(L"error opening file\n");
		return 1;
	}

	file_size = GetFileSize(file, NULL);

	if(file_size < EMMC_SECTOR_SIZE) 
	{
		CloseHandle(file);

		wprintf(L"failed: file is too small)\n");
		return 1;
	}

	if(file_size % EMMC_SECTOR_SIZE != 0)
	{
		CloseHandle(file);

		wprintf(L"failed: file size must be multiply of sector size (%u)\n", (unsigned int) EMMC_SECTOR_SIZE);
		return 1;
	}

	mb->data = (void *) HeapAlloc(GetProcessHeap(), 0, file_size);

	if(mb->data == NULL)
	{
		CloseHandle(file);

		wprintf(L"error loading file (can't allocate memory)\n");
		return 1;
	}

	if(ReadFile(file, mb->data, file_size, &bytes_returned, NULL) == TRUE && bytes_returned == file_size)
	{
		CloseHandle(file);
		mb->length = file_size;

		wprintf(L"%u bytes was successfully loaded from file!\n", bytes_returned);
		return 0;
	}

	CloseHandle(file);
	wprintf(L"error reading from file\n");
	return 1;
}

int save_file(struct mem_block *mb, wchar_t *file_name)
{
	HANDLE file;
	DWORD bytes_written;
	BOOL result;

	wprintf(L"Saving data to file... ");

	file = CreateFile(file_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);

	if(file == INVALID_HANDLE_VALUE)
	{
		wprintf(L"error creating file\n");
		return 1;
	}

	result = WriteFile(file, mb->data, mb->length, &bytes_written, NULL);
	CloseHandle(file);

	if(result == FALSE || bytes_written != mb->length)
	{
		wprintf(L"error writing to file\n");
		return 1;
	}

	wprintf(L"OK! %u bytes written\n", bytes_written);
	return 0;
}

int check_gpt_header(struct gpt_header *hdr)
{
	wprintf(L"Checking GPT header... ");

	if(memcmp(hdr->signature, "EFI PART", 8) != 0)
	{
		wprintf(L"\"EFI PART\" signature not detected\n");
		return 1;
	}

	if(hdr->revision !=  0x00010000)
	{
		wprintf(L"invalid \"revision\" value\n");
		return 1;
	}

	if(hdr->size_of_partition_entry != GPT_PART_ENTRY_SIZE)
	{
		wprintf(L"unsupported partition entry size: %u\n", hdr->size_of_partition_entry);
		return 1;
	}

	wprintf(L"OK!\n");
	return 0;
}

void do_realign(struct gpt_header *hdr, struct gpt_part_entry *p, unsigned int alignment_bytes, unsigned __int64 disk_size_sectors)
{
	unsigned __int64 alignment;
	unsigned __int64 part_length_sectors;
	unsigned __int64 next_usable_sector;
	wchar_t part_name[PART_NAME_SIZE];
	unsigned int i;
	unsigned char null_guid[GUID_SIZE] = { 0 };

	alignment = (unsigned __int64) (alignment_bytes / EMMC_SECTOR_SIZE); /* For 8MB and 512 bytes sector length, this value will be 16384 */

	wprintf(L"Re-calculating partition table (alignment: %I64u sectors / %u bytes)...\n\n", alignment, alignment_bytes);

	next_usable_sector = FIRST_USABLE_SECTOR;

	for(i = 0; i < hdr->num_of_partition_entries; i++)
	{
		if(memcmp((void *) p->part_type_guid, (void *) null_guid, GUID_SIZE) == 0)
		{
			wprintf(L"Processing partition %u: NOT USED\n\n", (unsigned int) i + 1);
			continue;
		}

		memset(part_name, 0, PART_NAME_SIZE * sizeof(wchar_t));
		wcscpy_s(part_name, PART_NAME_SIZE, p->part_name);
		part_name[PART_NAME_SIZE - 1] = L'\0'; /* Zero-terminated unicode string */

		part_length_sectors = (p->ending_lba + 1) - p->starting_lba;

		wprintf(L"Processing partition %u: %s...\nLength: %I64u sectors\n", i + 1, part_name, part_length_sectors);

		wprintf(L"First:  %I64u -> ", p->starting_lba);

		if(next_usable_sector % alignment == 0)
		{
			p->starting_lba = next_usable_sector;
			wprintf(L"%I64u (no gap from prev. part.)\nLast:   %I64u -> ", p->starting_lba, p->ending_lba);
		}
		else
		{
			p->starting_lba = ((next_usable_sector / alignment) + 1) * alignment; /* Let's do some magic! */
			wprintf(L"%I64u (%I64u unused sectors from prev. part.)\nLast:   %I64u -> ", p->starting_lba, p->starting_lba - next_usable_sector, p->ending_lba);
		}

		if(wcscmp(p->part_name, L"userdata") == 0) /* userdata usually comes after all other partitions */
		{
			if(disk_size_sectors % alignment == 0)
				p->ending_lba = (disk_size_sectors - alignment) - 1;
			else /* TODO: add more advanced code */
				p->ending_lba = (((disk_size_sectors - (alignment * 2)) / alignment) * alignment) - 1;

			wprintf(L"%I64u (expanded to fill free space)\nstart_byte_hex=0x%I64x\n\n", p->ending_lba, (unsigned __int64) (p->starting_lba * EMMC_SECTOR_SIZE));
		}
		else /* other partitions (sbl, tz, boot, rpm, ...) */
		{
			p->ending_lba = p->starting_lba + part_length_sectors - 1;
			wprintf(L"%I64u\nstart_byte_hex=0x%I64x\n\n", p->ending_lba, (unsigned __int64) (p->starting_lba * EMMC_SECTOR_SIZE));
		}

		next_usable_sector = p->ending_lba + 1;
		p++; /* Go to next item */
	}
}

unsigned int get_num_of_unused_entries(struct gpt_header *hdr, struct gpt_part_entry *pt)
{
	unsigned int i;
	unsigned int unused_count = 0;
	unsigned char null_guid[GUID_SIZE] = { 0 }; /* From UEFI spec.: 'A value of zero defines that this partition entry is not being used' */

	for(i = 0; i < hdr->num_of_partition_entries; i++)
	{
		if(memcmp(pt->part_type_guid, null_guid, GUID_SIZE) == 0)
			unused_count++;

		pt++;
	}

	return unused_count;
}

int patch_main(wchar_t *file_name, unsigned int alignment, __int64 disk_size)
{
	struct mem_block mb;
	unsigned char *data;
	struct gpt_header *gpt_hdr;
	struct gpt_part_entry *part_entries; /* Pointer to first element */
	unsigned int num_of_unused;

	wprintf(L"=== Patching gpt_main0.bin (%s) ===\n", file_name);

	if(load_file(&mb, file_name))
		return 1;

	if(mb.length != MAIN_SIZE_BYTES)
		wprintf(L"WARNING: File size differs from pre-defined\n");

	data = (unsigned char *) mb.data; /* For easy offset calculation, we can represent data as array of bytes */

	gpt_hdr = (struct gpt_header *) &data[EMMC_SECTOR_SIZE]; /* Primary (main) GPT header must be located at LBA 1 */
	part_entries = (struct gpt_part_entry *) &data[EMMC_SECTOR_SIZE * 2];

	if(check_gpt_header(gpt_hdr))
	{
		HeapFree(GetProcessHeap(), 0, mb.data);
		return 1;
	}

	if(gpt_hdr->my_lba != 1) /* For main, this value must be set to ONE */
		wprintf(L"WARNING: \"My LBA\" != 1\n");

	if(gpt_hdr->first_usable_lba != MAIN_SIZE_SECTORS)
		wprintf(L"WARNING: First usable LBA differs from pre-defined\n");

	num_of_unused = get_num_of_unused_entries(gpt_hdr, part_entries);
	wprintf(L"Partition entries used: %u/%u\n", gpt_hdr->num_of_partition_entries - num_of_unused, gpt_hdr->num_of_partition_entries);

	do_realign(gpt_hdr, part_entries, alignment, disk_size);

	gpt_hdr->alternate_lba = disk_size - 1;
	wprintf(L"Location of alternate (backup) header: %I64d\n", gpt_hdr->alternate_lba);

	gpt_hdr->last_usable_lba = (disk_size - BACKUP_SIZE_SECTORS) - 1;
	wprintf(L"Updated \"Last usable LBA\": %I64d\n", gpt_hdr->last_usable_lba);

	chksum_crc32gentab(); /* Dont forget initialization of CRC table */
	gpt_hdr->part_entry_array_crc32 = chksum_crc32 ((unsigned char *) part_entries, gpt_hdr->num_of_partition_entries * GPT_PART_ENTRY_SIZE);

	chksum_crc32gentab(); /* Dont forget initialization of CRC table */
	gpt_hdr->header_crc32 = 0x00000000;
	gpt_hdr->header_crc32 = chksum_crc32 ((unsigned char *) gpt_hdr, GPT_HEADER_SIZE);

	wprintf(L"CRC32 (partitions): %8X\nCRC32 (GPT header): %8X\n\n", gpt_hdr->part_entry_array_crc32, gpt_hdr->header_crc32);

	save_file(&mb, file_name);

	HeapFree(GetProcessHeap(), 0, mb.data);
	return 0;
}

int patch_backup(wchar_t *file_name, unsigned int alignment, __int64 disk_size)
{
	struct mem_block mb;
	unsigned char *data;
	struct gpt_header *gpt_hdr;
	struct gpt_part_entry *part_entries; /* Pointer to first element */
	unsigned int num_of_unused;

	wprintf(L"=== Patching gpt_backup0.bin (%s) ===\n", file_name);

	if(load_file(&mb, file_name))
		return 1;

	if(mb.length != BACKUP_SIZE_BYTES)
		wprintf(L"WARNING: File size differs from pre-defined\n");

	data = (unsigned char *) mb.data; /* For easy offset calculation, we can represent data as array of bytes */

	gpt_hdr = (struct gpt_header *) &data[mb.length - EMMC_SECTOR_SIZE]; /* Primary (main) GPT header must be located at last LBA */
	part_entries = (struct gpt_part_entry *) data;

	if(check_gpt_header(gpt_hdr))
	{
		HeapFree(GetProcessHeap(), 0, mb.data);
		return 1;
	}

	if(gpt_hdr->alternate_lba != 1)
		wprintf(L"WARNING: \"Alternate LBA\" != 1\n");

	if(gpt_hdr->first_usable_lba != MAIN_SIZE_SECTORS)
		wprintf(L"WARNING: First usable LBA differs from pre-defined\n");

	num_of_unused = get_num_of_unused_entries(gpt_hdr, part_entries);
	wprintf(L"Partition entries used: %u/%u\n", gpt_hdr->num_of_partition_entries - num_of_unused, gpt_hdr->num_of_partition_entries);

	do_realign(gpt_hdr, part_entries, alignment, disk_size);

	gpt_hdr->my_lba = disk_size - 1;
	wprintf(L"Location of this (backup) header: %I64d\n", gpt_hdr->my_lba);

	gpt_hdr->partition_entry_lba = disk_size - (unsigned __int64) (mb.length / EMMC_SECTOR_SIZE);
	wprintf(L"Partition entry array location:   %I64d\n", gpt_hdr->partition_entry_lba);

	gpt_hdr->last_usable_lba = gpt_hdr->partition_entry_lba - 1;
	wprintf(L"Updated \"Last usable LBA\":      %I64d\n", gpt_hdr->last_usable_lba);

	chksum_crc32gentab(); /* Dont forget initialization of CRC table */
	gpt_hdr->part_entry_array_crc32 = chksum_crc32 ((unsigned char *) part_entries, gpt_hdr->num_of_partition_entries * GPT_PART_ENTRY_SIZE);

	chksum_crc32gentab(); /* Dont forget initialization of CRC table */
	gpt_hdr->header_crc32 = 0x00000000;
	gpt_hdr->header_crc32 = chksum_crc32 ((unsigned char *) gpt_hdr, GPT_HEADER_SIZE);

	wprintf(L"CRC32 (partitions): %8X\nCRC32 (GPT header): %8X\n\n", gpt_hdr->part_entry_array_crc32, gpt_hdr->header_crc32);

	save_file(&mb, file_name);

	HeapFree(GetProcessHeap(), 0, mb.data);
	return 0;
}


int do_internal_test()
{
	if(sizeof(unsigned char) != 1)
	{
		wprintf(L"Internal test failed: sizeof(unsigned char) != 1\n");
		return 1;
	}

	if(sizeof(wchar_t) != 2)
	{
		wprintf(L"Internal test failed: sizeof(wchar_t) != 2\n");
		return 1;
	}

	if(sizeof(unsigned int) != 4)
	{
		wprintf(L"Internal test failed: sizeof(unsigned int) != 4\n");
		return 1;
	}

	if(sizeof(unsigned __int64) != 8)
	{
		wprintf(L"Internal test failed: sizeof(unsigned __int64) != 8\n");
		return 1;
	}

	if(sizeof(struct gpt_header) != GPT_HEADER_SIZE)
	{
		wprintf(L"Internal test failed: bad GPT header struct size (%u)\n", (unsigned int) sizeof(struct gpt_header));
		return 1;
	}

	if(sizeof(struct gpt_part_entry) != GPT_PART_ENTRY_SIZE)
	{
		wprintf(L"Internal test failed: bad GPT part.entry struct size (%u)\n", (unsigned int) sizeof(struct gpt_part_entry));
		return 1;
	}

	return 0;
}

unsigned int get_alignment(wchar_t *argument)
{
	if(wcscmp(argument, L"256K") == 0)
		return 262144;

	if(wcscmp(argument, L"512K") == 0)
		return 524288;

	if(wcscmp(argument, L"1M") == 0)
		return 1048576;

	if(wcscmp(argument, L"2M") == 0)
		return 2097152;

	if(wcscmp(argument, L"4M") == 0)
		return 4194304;

	if(wcscmp(argument, L"8M") == 0)
		return 8388608;

	if(wcscmp(argument, L"16M") == 0)
		return 16777216;

	return 0; /* Unsupported value */
}

int wmain(int argc, wchar_t *argv[], wchar_t *envp[])
{
	unsigned int alignment;
	__int64 disk_size;

	wprintf(L"msm8916-realign for Xiaomi Redmi 2\nThis software comes with no warranties, use it at your own risk!\n\n");

	if(do_internal_test())
	{
		wprintf(L"Please re-compile application with correct compiler/settings\n");
		return 1;
	}

	if(argc != 5)
	{
		wprintf(L"Usage: msm8916-realign <alignment> <disk size sectors> <file type> <file name>\n\n");
		wprintf(L"Where <alignment> can be 256K / 512K / 1M / 2M / 4M / 8M / 16M\n");
		wprintf(L"  and <file type> must be set to one of two values: main / backup\n\n");
		wprintf(L"Example: msm8916-realign 8M 15302656 main gpt_main0.bin\n");
		wprintf(L"         msm8916-realign 8M 15302656 backup gpt_backup0.bin\n");
		return 0;
	}

	alignment = get_alignment(argv[1]);

	if(alignment == 0)
	{
		wprintf(L"Incorrect or unsupported \"alignment\": %s\n", argv[1]);
		return 1;
	}

	disk_size = _wtoi64(argv[2]);

	if(disk_size < (__int64) (alignment / EMMC_SECTOR_SIZE))
	{
		wprintf(L"Incorrect disk size: %I64d\n", disk_size);
		return 1;
	}

	if(wcscmp(argv[3], L"main") == 0)
		return patch_main(argv[4], alignment, disk_size);

	if(wcscmp(argv[3], L"backup") == 0)
		return patch_backup(argv[4], alignment, disk_size);

	wprintf(L"Unknown file type: %s\n", argv[3]);
	return 0;
}
