/* 生成文件 —— 不要手改。
 * 来源: vmlinux.btf
 * 生成: tools/gen_offsets.py
 *
 * 这些偏移取自目标内核自己的 BTF，不同内核版本会不同。
 * 换内核后必须重新生成，否则模块会按错误布局读写内核内存。
 */
#ifndef CR_OFFSETS_H
#define CR_OFFSETS_H

#define CR_PDE_SIZE       176u
#define CR_PDE_PROCOPS    48u
#define CR_PDE_PARENT     120u
#define CR_PDE_SUBDIR     128u
#define CR_PDE_SUBDIRNODE 136u
#define CR_PDE_NAME       160u
#define CR_PDE_MODE       168u
#define CR_PDE_FLAGS      170u
#define CR_PDE_NAMELEN    171u
#define CR_POPS_SIZE      96u
#define CR_POPS_OPEN      8u
#define CR_POPS_READ      16u
#define CR_POPS_READITER  24u
#define CR_POPS_WRITE     32u
#define CR_POPS_LSEEK     40u
#define CR_POPS_RELEASE   48u

#endif /* CR_OFFSETS_H */
