#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fsutil.h>

int main(void)
{
    char        root[] = "/tmp/fsutil-smoke-XXXXXX";
    fs_dir_t    top;
    fs_dir_t    data;
    fs_expect_t private_dir = fs_expect_private(0700);
    int         fd          = -1;

    assert(mkdtemp(root) != NULL);
    fs_dir_init(&top);
    fs_dir_init(&data);

    assert(fs_dir_open_abs_nofollow(root, NULL, &top) == 0);
    assert(fs_dir_walk_create(&top, "./data/store", 0700, &private_dir, &data) == 0);
    assert(fs_file_create_write_new_at(&data, "marker.tmp", 0600, NULL, &fd) == 0);
    assert(write(fd, "ok", 2) == 2);
    assert(fs_file_fsync(fd) == 0);
    assert(close(fd) == 0);
    assert(fs_rename_noreplace_at(&data, "marker.tmp", &data, "marker") == 0);
    assert(fs_rename_noreplace_at(&data, "marker", &data, "marker") == -EEXIST);
    assert(fs_dir_fsync(&data) == 0);
    assert(fs_unlink_at(&data, "marker") == 0);

    fs_dir_close(&data);
    fs_dir_close(&top);
    printf("smoke test: installed package round-trips correctly\n");
    return 0;
}
