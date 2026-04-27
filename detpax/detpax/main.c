#include <windows.h>
#include <stdio.h>
#include <locale.h>

const char* precPaths[] = {
    "skelpkg"
};

#define PREC_COUNT 1

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

typedef struct {
    char name[MAX_PATH];
    char fullpath[MAX_PATH];
    int dir;
    int order;

    unsigned long long dev;
    unsigned long long ino;

    int is_hardlink;
    int link_to_index;

} FileInfo;

typedef struct {
    unsigned long long dev;
    unsigned long long ino;
    int index;
} InodeMap;

InodeMap inode_map[10000];
int inode_count = 0;

#define MAX_FILES 10000

FileInfo files[MAX_FILES];
int fileCount = 0;

int fatal_error = 0;

FILE* out;

int get_inode(const char* path, unsigned long long* dev, unsigned long long* ino)
{
    HANDLE h = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );

    if (h == INVALID_HANDLE_VALUE)
        return 0;

    BY_HANDLE_FILE_INFORMATION info;

    if (!GetFileInformationByHandle(h, &info)) {
        CloseHandle(h);
        return 0;
    }

    *dev = info.dwVolumeSerialNumber;
    *ino = ((unsigned long long)info.nFileIndexHigh << 32) | info.nFileIndexLow;

    CloseHandle(h);
    return 1;
}

int find_inode(unsigned long long dev, unsigned long long ino)
{
    for (int i = 0; i < inode_count; i++) {
        if (inode_map[i].dev == dev &&
            inode_map[i].ino == ino)
            return inode_map[i].index;
    }
    return -1;
}

int is_prec_path(const char* path)
{
    for (int i = 0; i < PREC_COUNT; i++) {
        size_t len = strlen(precPaths[i]);

        if (strncmp(path, precPaths[i], len) == 0 &&
            (path[len] == '/' || path[len] == '\0'))
            return 1;
    }
    return 0;
}

int compute_order(const char* path, int isDir)
{
    if (is_prec_path(path))
        return 0;

    if (isDir)
        return 1;

    return 2;
}

int compare(const void* a, const void* b)
{
    FileInfo* fa = (FileInfo*)a;
    FileInfo* fb = (FileInfo*)b;

    if (fa->order != fb->order)
        return fa->order - fb->order;

    return strcmp(fa->name, fb->name);
}

char* normalize_path(char* s)
{
    for (char* p = s; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
    return s;
}

void tar_write_header(FILE* out, const char* filename, long size, char typeflag, const char* linkname)
{
    struct tar_header h;
    memset(&h, 0, sizeof(h));

    if (typeflag == '5')
        snprintf(h.name, sizeof(h.name), "%s/", filename);
    else
        snprintf(h.name, sizeof(h.name), "%s", filename);

    snprintf(h.mode, 8, "%07o", typeflag == '5' ? 0755 : 0644);

    snprintf(h.uid, 8, "%07o", 0);
    snprintf(h.gid, 8, "%07o", 0);

    snprintf(h.size, 12, "%011lo", (typeflag == '0') ? size : 0);

    snprintf(h.mtime, 12, "%011o", 347155200);

    h.typeflag = typeflag;

    if (linkname)
        snprintf(h.linkname, sizeof(h.linkname), "%s", linkname);

    memcpy(h.magic, "ustar", 5);
    h.magic[5] = '\0';

    memcpy(h.version, "00", 2);

    memset(h.checksum, ' ', 8);

    unsigned int sum = 0;
    unsigned char* p = (unsigned char*)&h;
    for (int i = 0; i < 512; i++) sum += p[i];
        snprintf(h.checksum, 8, "%06o", sum);

    h.checksum[6] = '\0';
    h.checksum[7] = ' ';

    fwrite(&h, 1, 512, out);
}

void tar_write_file(FILE* out, const char* path, const char* name)
{
    FILE* f = fopen(path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    tar_write_header(out, name, size, '0', NULL);

    char buf[4096];
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, out);

    long pad = (512 - (size % 512)) % 512;
    char zero[512] = { 0 };
    fwrite(zero, 1, pad, out);

    fclose(f);
}

void tar_write_dir(FILE* out, const char* name)
{
    tar_write_header(out, name, 0, '5', NULL);
}

void walk(const char* path, const char* root)
{
    if (fatal_error)
        return;

    WIN32_FIND_DATAA findFileData;
    HANDLE hFind;

    char searchPath[MAX_PATH];
    snprintf(searchPath, MAX_PATH, "%s\\*", path);

    hFind = FindFirstFileA(searchPath, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        printf("Не удалось открыть папку: %s\n", path);
        return;
    }

    do {
        const char* name = findFileData.cFileName;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        char fullPath[MAX_PATH];
        snprintf(fullPath, MAX_PATH, "%s\\%s", path, name);

        if (fileCount >= MAX_FILES) {
            printf("Слишком много файлов!\n");
            FindClose(hFind);
            return;
        }

        int index = fileCount;
        FileInfo* info = &files[fileCount++];

        char* rel;
        rel = fullPath + strlen(root) + 1;
        normalize_path(rel);

        strncpy_s(info->fullpath, MAX_PATH, fullPath, _TRUNCATE);
        strncpy_s(info->name, MAX_PATH, rel, _TRUNCATE);

        info->is_hardlink = 0;
        info->link_to_index = -1;

        info->dir = (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

        if (inode_count >= 10000) {
            printf("Слишком много inode!\n");
            fatal_error = 1;
            break;
        }

        if (!info->dir) {
            unsigned long long dev, ino;

            if (get_inode(fullPath, &dev, &ino)) {
                int first = find_inode(dev, ino);

                info->dev = dev;
                info->ino = ino;

                if (first == -1) {
                    inode_map[inode_count++] = (InodeMap){ dev, ino, index };
                    info->is_hardlink = 0;
                }
                else {
                    info->is_hardlink = 1;
                    info->link_to_index = first;
                }
            }

            else
            {
                info->dev = 0;
                info->ino = 0;
            }
        }
        else
        {
            info->dev = 0;
            info->ino = 0;
        }

        info->order = compute_order(rel, info->dir);

        if (info->dir)
        {
            walk(fullPath, root);
        }

    } while (FindNextFileA(hFind, &findFileData));

    FindClose(hFind);
}

int main(int argc, char* argv[]) {

    setlocale(LC_ALL, "");

    if (argc < 2) {
        printf("Использование: program.exe <путь_к_папке>\n");
        return 1;
    }

    const char* folder = argv[1];

    char rootPath[MAX_PATH];

    strncpy_s(rootPath, MAX_PATH, folder, _TRUNCATE);
    normalize_path(rootPath);

    size_t len = strlen(rootPath);
    if (len > 0 && (rootPath[len - 1] == '/' || rootPath[len - 1] == '\\')) {
        rootPath[len - 1] = '\0';
    }

    walk(folder, rootPath);

    fopen_s(&out, "out.tar", "wb");
    if (!out) return 1;

    qsort(files, fileCount, sizeof(FileInfo), compare);

    for (int i = 0; i < fileCount; i++)
    {
        if (files[i].dir)
            tar_write_dir(out, files[i].name);
        else if (files[i].is_hardlink)
        {
            const char* target = files[files[i].link_to_index].name;
            tar_write_header(out, files[i].name, 0, '1', target);
        }
        else
        {
            tar_write_file(out, files[i].fullpath, files[i].name);
        }

        printf("[%s] %s\n", files[i].dir ? "DIR" : "FILE", files[i].name);
    }

    char zero[1024] = { 0 };
    fwrite(zero, 1, 1024, out);

    return 0;
}