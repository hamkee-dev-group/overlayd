#ifndef OVERLAYD_TAR_H
#define OVERLAYD_TAR_H

int tar_create(const char *src_dir, const char *out_path);
int tar_extract(const char *tar_path, const char *dst_dir);

#endif
