/* Unity 在这套 NVN 名字表里唯一没有实现、却真会被调到的入口。
 *
 * 共享的 d2r_gfx 把 523 个名字分成三类：有语义的、录成命令的、以及只加载不调用的。Unity 不属于第三类——
 * 共享字体就位之后它马上开始出图，第一个打到的空实现就是 nvnCommandBufferDownsample，计数跳板落进
 * nvn_unimpl_c 直接终止进程。
 *
 * 语义上它是"把源整幅降采样进目标"：两端各自是一个贴图的第 0 级，尺寸按倍率缩小，或者尺寸相同只把多重采样
 * 解析成单采样（Unity 走的是后者，2 倍多重采样的渲染目标每帧末尾解到出图贴图上）。命令层里已经有贴图→贴图
 * 拷贝这一种记录（OP_COPY_T2T），调度、贴图钉住、事件依赖、写回账本都按它算好了；差别只在两端尺寸或采样数
 * 不等，而 nvn_mtl.m 的 copy_t2t() 把这两种形态分别交给带线性过滤的缩放解析和一次只有解析的渲染通道。
 * 于是这里只做一件事：按整幅区域把调用录成一条拷贝，让它留在提交顺序里——同一个命令缓冲里排在它前面
 * 画进源的东西，回放时自然先生效。
 */
#include "runtime.h"
#include <stdio.h>
#include "../../refs/by3-port/games/totk/third_party/d2r_gfx/gfx/nvn/nvn_obj.h"
#include "../../refs/by3-port/games/totk/third_party/d2r_gfx/gfx/nvn/nvn_impl.h"

/* hle_nvn.c: 按名字取共享表里已经实现的入口（NVN_EXTRA 的转发也走它） */
void *nvn_slot_fn(const char *name);

typedef void (*copy_t2t_fn)(NVNcommandBuffer *, const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *,
                            const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *, int);

/* NVN 没有公开头文件，方向不按印象猜，按数据定：降采样只可能从多采样解到少采样、从大尺寸缩到小尺寸；
   两端完全一致时谁当源都等价。首次调用把两端描述打出来——判出来的方向不对，在这里看得很清楚。
   录制用 nvnCommandBufferCopyTextureToTexture，它的参数是源在前、目标在后（hle_nvn.c:1872）。 */
NVN_NOPROTO(nvnCommandBufferDownsample, void, NVNcommandBuffer *c, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *ta = (const tex_t *)a, *tb = (const tex_t *)b;
    int a_is_src = ta->d.samples != tb->d.samples ? ta->d.samples > tb->d.samples
                  : (ta->d.width > tb->d.width || (ta->d.width == tb->d.width && ta->d.height >= tb->d.height));
    const tex_t *src = a_is_src ? ta : tb, *dst = a_is_src ? tb : ta;
    { static int once;
      if (!once++) fprintf(stderr, "[dora-nvn] Downsample 参数一 %ux%ux%u 采样 %d 级 %u 格式 %#x 标志 %#x（serial %llu）"
                                    "参数二 %ux%ux%u 采样 %d 级 %u 格式 %#x 标志 %#x（serial %llu）→ 源取参数%d、目标取参数%d\n",
          ta->d.width, ta->d.height, ta->d.depth, ta->d.samples, ta->d.levels, ta->d.format, ta->d.flags, (unsigned long long)ta->serial,
          tb->d.width, tb->d.height, tb->d.depth, tb->d.samples, tb->d.levels, tb->d.format, tb->d.flags, (unsigned long long)tb->serial,
          a_is_src ? 1 : 2, a_is_src ? 2 : 1);
      if (src->d.samples <= dst->d.samples && src->d.width < dst->d.width)
          fprintf(stderr, "[dora-nvn] Downsample 判出来的源比目标小：两端描述可能不足以判断方向\n"); }
    copy_t2t_fn copy = (copy_t2t_fn)nvn_slot_fn("nvnCommandBufferCopyTextureToTexture");
    if (!copy) { fprintf(stderr, "[dora-nvn] CopyTextureToTexture 不在表里，Downsample 无法录制\n"); return; }
    NVNcopyRegion sr = {0, 0, 0, (int)src->d.width, (int)src->d.height, (int)src->d.depth ? (int)src->d.depth : 1};
    NVNcopyRegion dr = {0, 0, 0, (int)dst->d.width, (int)dst->d.height, (int)dst->d.depth ? (int)dst->d.depth : 1};
    copy(c, (const NVNtexture *)src, NULL, &sr, (const NVNtexture *)dst, NULL, &dr, 0);
}
