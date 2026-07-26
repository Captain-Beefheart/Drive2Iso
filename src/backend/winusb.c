#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "winusb.h"
#include "drive2iso/util.h"

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOL 26

struct winusb_dev {
    HANDLE phys;
    HANDLE vols[MAX_VOL];
    int    nvol;
};

static HANDLE open_volume(char letter, DWORD access) {
    char path[8];
    snprintf(path, sizeof path, "\\\\.\\%c:", letter);
    return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static HANDLE open_physical(int drive, DWORD access) {
    char path[32];
    snprintf(path, sizeof path, "\\\\.\\PhysicalDrive%d", drive);
    return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static int volume_device_number(HANDLE h) {
    STORAGE_DEVICE_NUMBER sdn;
    DWORD ret = 0;
    memset(&sdn, 0, sizeof sdn);
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                        &sdn, sizeof sdn, &ret, NULL))
        return (int)sdn.DeviceNumber;
    return -1;
}

int winusb_is_removable(int drive) {
    HANDLE h = open_physical(drive, 0);
    if (h == INVALID_HANDLE_VALUE) return -1;
    STORAGE_PROPERTY_QUERY q;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    char buf[512];
    DWORD ret = 0;
    int r = -1;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                        buf, sizeof buf, &ret, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        r = d->RemovableMedia ? 1 : 0;
    }
    CloseHandle(h);
    return r;
}

uint64_t winusb_size(int drive) {
    HANDLE h = open_physical(drive, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD ret = 0;
    uint64_t sz = 0;
    GET_LENGTH_INFORMATION li;
    memset(&li, 0, sizeof li);
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                        &li, sizeof li, &ret, NULL))
        sz = (uint64_t)li.Length.QuadPart;
    if (sz == 0) {
        /* GET_LENGTH_INFO needs read access; geometry works unprivileged. */
        BYTE buf[512];
        memset(buf, 0, sizeof buf);
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                            buf, sizeof buf, &ret, NULL))
            sz = (uint64_t)((DISK_GEOMETRY_EX *)buf)->DiskSize.QuadPart;
    }
    CloseHandle(h);
    return sz;
}

static char sysdrive_letter(void) {
    const char *sd = getenv("SystemDrive");
    return (sd && sd[0]) ? sd[0] : 'C';
}

int winusb_hosts_system(int drive) {
    HANDLE h = open_volume(sysdrive_letter(), 0);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int dn = volume_device_number(h);
    CloseHandle(h);
    if (dn < 0) return -1;
    return dn == drive ? 1 : 0;
}

static void query_model(int drive, char *out, size_t outsz) {
    out[0] = '\0';
    HANDLE h = open_physical(drive, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    STORAGE_PROPERTY_QUERY q;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    char buf[1024];
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q,
                        buf, sizeof buf, &ret, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        const char *vendor  = d->VendorIdOffset  ? buf + d->VendorIdOffset  : "";
        const char *product = d->ProductIdOffset ? buf + d->ProductIdOffset : "";
        snprintf(out, outsz, "%s %s", vendor, product);
    }
    CloseHandle(h);
}

void winusb_list(void) {
    printf("  #   SIZE          REMOVABLE  SYSTEM  MODEL\n");
    for (int i = 0; i < 16; ++i) {
        HANDLE h = open_physical(i, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);
        uint64_t sz = winusb_size(i);
        int rem = winusb_is_removable(i);
        int sys = winusb_hosts_system(i);
        char model[128];
        query_model(i, model, sizeof model);
        printf("  %-3d %8llu MiB  %-9s  %-6s  %s\n",
               i, (unsigned long long)(sz / (1024ull * 1024ull)),
               rem == 1 ? "yes" : rem == 0 ? "no" : "?",
               sys == 1 ? "YES" : "no", model);
    }
}

winusb_dev *winusb_open(int drive, char **err) {
    winusb_dev *d = calloc(1, sizeof *d);
    if (!d) { if (err) *err = xstrdup("out of memory"); return NULL; }
    d->phys = INVALID_HANDLE_VALUE;

    /* Lock + dismount every volume that lives on this physical drive. */
    DWORD mask = GetLogicalDrives();
    for (char c = 'A'; c <= 'Z'; ++c) {
        if (!(mask & (1u << (c - 'A')))) continue;
        HANDLE v = open_volume(c, GENERIC_READ | GENERIC_WRITE);
        if (v == INVALID_HANDLE_VALUE) continue;
        if (volume_device_number(v) != drive) { CloseHandle(v); continue; }
        DWORD ret = 0;
        BOOL locked = FALSE;
        for (int t = 0; t < 10 && !locked; ++t) {
            locked = DeviceIoControl(v, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL);
            if (!locked) Sleep(100);
        }
        DeviceIoControl(v, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL);
        if (d->nvol < MAX_VOL) d->vols[d->nvol++] = v;
        else CloseHandle(v);
    }

    HANDLE p = open_physical(drive, GENERIC_READ | GENERIC_WRITE);
    if (p == INVALID_HANDLE_VALUE) {
        if (err) *err = str_format("cannot open PhysicalDrive%d (elevated? drive in use?)", drive);
        winusb_close(d);
        return NULL;
    }
    DWORD ret = 0;
    DeviceIoControl(p, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &ret, NULL);
    d->phys = p;
    return d;
}

long winusb_write(winusb_dev *d, const void *buf, size_t len) {
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
        DWORD wr = 0;
        if (!WriteFile(d->phys, p + off, (DWORD)(len - off), &wr, NULL)) return -1;
        if (wr == 0) return -1;
        off += wr;
    }
    return (long)off;
}

long winusb_read(winusb_dev *d, void *buf, size_t len) {
    char *p = buf;
    size_t off = 0;
    while (off < len) {
        DWORD rd = 0;
        if (!ReadFile(d->phys, p + off, (DWORD)(len - off), &rd, NULL)) return -1;
        if (rd == 0) return -1;
        off += rd;
    }
    return (long)off;
}

int winusb_rewind(winusb_dev *d) {
    LARGE_INTEGER z;
    z.QuadPart = 0;
    return SetFilePointerEx(d->phys, z, NULL, FILE_BEGIN) ? 0 : -1;
}

void winusb_close(winusb_dev *d) {
    if (!d) return;
    DWORD ret = 0;
    for (int i = 0; i < d->nvol; ++i) {
        if (d->vols[i] && d->vols[i] != INVALID_HANDLE_VALUE) {
            DeviceIoControl(d->vols[i], FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL);
            CloseHandle(d->vols[i]);
        }
    }
    if (d->phys && d->phys != INVALID_HANDLE_VALUE) CloseHandle(d->phys);
    free(d);
}
