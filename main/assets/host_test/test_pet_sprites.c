#include <stdio.h>
#include "pet_sprites.h"
#include "tribe_icon_pack.h"
#include "tribe_icons_mayday.h"
int main(void){
    // 阈值单调性 + 形态推导正确性，逐个地点数实测
    int expect[]={0,0,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5};
    int bad=0;
    for(int p=0;p<20;p++){
        int f=pet_form_for_places((uint8_t)p);
        if(f!=expect[p]){printf("FAIL places=%d got=%d want=%d\n",p,f,expect[p]);bad++;}
    }
    printf("form mapping: %s\n", bad?"FAIL":"PASS (20 cases)");
    // 每个形态的 header 与 anchor 边界
    for(int i=0;i<PET_FORM_COUNT;i++){
        const lv_image_dsc_t*d=pet_form_sprites[i];
        int ok = d->header.w==PET_FORM_W && d->header.h==PET_FORM_H
              && d->header.cf==LV_COLOR_FORMAT_RGB565A8
              && d->data_size==(uint32_t)(PET_FORM_W*PET_FORM_H*3);
        int ax=pet_form_anchors[i].x, ay=pet_form_anchors[i].y;
        int inb = ax>=0&&ay>=0&&ax+PET_ACC_W<=PET_FORM_W&&ay+PET_ACC_H<=PET_FORM_H;
        printf("form %d hdr=%s size=%u anchor=(%d,%d) inbounds=%s\n",
               i, ok?"OK":"BAD", d->data_size, ax, ay, inb?"yes":"NO");
        if(!ok||!inb) bad++;
    }
    for(int i=0;i<PET_TRAIT_COUNT;i++){
        const lv_image_dsc_t*d=pet_acc_sprites[i];
        int ok=d->header.w==PET_ACC_W&&d->header.h==PET_ACC_H
             &&d->data_size==(uint32_t)(PET_ACC_W*PET_ACC_H*3);
        printf("acc  %d hdr=%s size=%u\n",i,ok?"OK":"BAD",d->data_size);
        if(!ok)bad++;
    }
    for(int i=0;i<TRIBE_ICON_COUNT;i++){
        const lv_image_dsc_t*d=tribe_icon_pack_mayday[i];
        if(!(d->header.w==32&&d->header.h==32&&d->data_size==3072u)){
            printf("icon %d BAD\n",i);bad++;}
    }
    if(tribe_icon_get(&tribe_icon_pack_active,99)!=
       tribe_icon_pack_mayday[TRIBE_ICON_CARROT]){
        printf("icon fallback BAD\n");bad++;
    }
    printf("icons: %s\n", bad?"see above":"PASS 8/8");
    printf("\n%s\n", bad?"RESULT: FAIL":"RESULT: ALL PASS");
    return bad?1:0;
}
