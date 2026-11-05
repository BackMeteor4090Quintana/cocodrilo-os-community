#pragma once
#include "lowzag.h"
#include <esp_heap_caps.h>
#include <string.h>
#include <stdint.h>

// Progressive-JPEG decoder bridge. Copyright COCODRILO OS contributors.
// lowzag is separately licensed under MIT (see lowzag.h).
namespace PortadaLowzag {
struct Ctx {
 const uint8_t* jpg; size_t len, pos;
 uint16_t* out; int width,height,ox,oy;
 volatile uint32_t* generation; uint32_t expected;
};
static int read(void* v,uint8_t* p,size_t n) {
 Ctx* c=(Ctx*)v; size_t take=n<c->len-c->pos?n:c->len-c->pos;
 if(take)memcpy(p,c->jpg+c->pos,take); c->pos+=take; return (int)take;
}
static int cancelled(void* v) {
 Ctx* c=(Ctx*)v; return *c->generation!=c->expected;
}
static void row(void* v,int y,int w,const uint8_t* rgb) {
 Ctx* c=(Ctx*)v; if(*c->generation!=c->expected)return;
 int yy=y+c->oy;if(yy<0||yy>=c->height)return;
 for(int x=0;x<w;x++){int xx=x+c->ox;if(xx<0||xx>=c->width)continue;
 const uint8_t* a=rgb+x*3;
 uint16_t native=((uint16_t)(a[0]&0xf8)<<8)|((uint16_t)(a[1]&0xfc)<<3)|(a[2]>>3);
 c->out[yy*c->width+xx]=(native>>8)|(native<<8);
 }
}
static void* alloc(void*,size_t n) {
 void*p=heap_caps_aligned_alloc(8,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 return p?p:heap_caps_aligned_alloc(8,n,MALLOC_CAP_8BIT);
}
static void free(void*,void*p){heap_caps_free(p);}
static bool decode(const uint8_t*jpg,size_t len,uint16_t*out,int outW,int outH,
                    volatile uint32_t*gen,uint32_t expected,int&jw,int&jh){
 if(lowzag_probe(jpg,len,&jw,&jh)!=LOWZAG_OK||jw<=0||jh<=0)return false;
 Ctx ctx={jpg,len,0,out,outW,outH,
      (outW-(jw+7)/8>0?(outW-(jw+7)/8)/2:0),
      (outH-(jh+7)/8>0?(outH-(jh+7)/8)/2:0),gen,expected};
 lowzag_cfg_t cfg={};cfg.struct_size=sizeof(cfg);
 cfg.read=read;cfg.read_ctx=&ctx;cfg.row=row;cfg.row_ctx=&ctx;
 cfg.cancel=cancelled;cfg.cancel_ctx=&ctx;cfg.alloc=alloc;cfg.free=free;
 cfg.force_scale=8;cfg.mem_limit=5U*1024U*1024U;
 lowzag_info_t info={};info.struct_size=sizeof(info);
 return lowzag_decode(&cfg,&info)==LOWZAG_OK && *gen==expected;
}
}
