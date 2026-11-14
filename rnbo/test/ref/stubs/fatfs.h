/* Host stand-in for FatFs: only the types the preset headers mention. */
#ifndef HOST_FATFS_H
#define HOST_FATFS_H
typedef enum { FR_OK = 0, FR_DISK_ERR } FRESULT;
typedef struct { int dummy; } FIL;
typedef struct { int dummy; } FATFS;
typedef struct { int dummy; } DIR;
typedef struct { int dummy; } FILINFO;
#endif
