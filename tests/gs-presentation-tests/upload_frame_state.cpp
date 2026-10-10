// SPDX-License-Identifier: GPL-3.0-or-later
// Compile the candidate's exact UploadFrame body with authored host adapters.
// This tests host cache state transitions, not Vulkan or OpenGL pixel conversion.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#define PS2_IF_AGRESSIVE_LOGS(...)
constexpr uint32_t FB_WIDTH=640,FB_HEIGHT=512,DEFAULT_DISPLAY_HEIGHT=448;
constexpr uint32_t DEFAULT_FB_SIZE=FB_WIDTH*FB_HEIGHT*4u;
constexpr unsigned MAGENTA=0xff00ffu;
unsigned neutralUploads=0,uploads=0,magentaImages=0;
struct Texture2D {std::vector<uint8_t> bytes;};
struct Image {std::vector<uint8_t> storage;void *data=nullptr;};
void UpdateTexture(Texture2D &texture,const void *data)
{
    const auto *bytes=static_cast<const uint8_t *>(data);
    texture.bytes.assign(bytes,bytes+DEFAULT_FB_SIZE);++uploads;
    if(std::all_of(texture.bytes.begin(),texture.bytes.end(),[](auto value){return value==0u;}))++neutralUploads;
}
Image GenImageColor(uint32_t,uint32_t,unsigned)
{++magentaImages;Image image;image.storage.assign(DEFAULT_FB_SIZE,255u);image.data=image.storage.data();return image;}
void UnloadImage(Image &){}
namespace ps2x_dev {std::vector<uint8_t> s_lastFrame;uint32_t s_lastFrameW=0,s_lastFrameH=0,s_lastFrameRows=0;}
struct FakeGs
{
    uint64_t reset=1;bool pending=true;unsigned latchCalls=0,copyCalls=0;
    uint8_t value=17;
    uint64_t hostPresentationResetEpoch() const{return reset;}
    bool latchHostPresentationFrame(){++latchCalls;return !pending;}
    bool copyLatchedHostPresentationFrame(std::vector<uint8_t> &out,uint32_t &w,uint32_t &h,
        uint32_t *display,uint32_t *source,bool *preferred)
    {++copyCalls;out.assign(DEFAULT_FB_SIZE,value);w=FB_WIDTH;h=FB_HEIGHT;*display=*source=0;*preferred=false;return true;}
};
struct FakeEe {uint64_t tick=0;uint64_t currentVSyncTick()const{return tick;}};
struct PS2Runtime {FakeGs graphics;FakeEe scheduler;FakeGs &gs(){return graphics;}FakeEe &eeScheduler(){return scheduler;}};
#include "upload_frame.inc"
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
int main()
{
    try
    {
        PS2Runtime runtime;Texture2D texture;uint32_t width=0,height=0;
        for(unsigned i=0;i<3;++i)UploadFrame(texture,&runtime,width,height);
        require(uploads==1 && neutralUploads==1 && magentaImages==0,"initial deferral did not retain one neutral upload");
        require(runtime.graphics.latchCalls==3 && runtime.graphics.copyCalls==0,"initial deferral did not retry same tick");
        require(width==FB_WIDTH && height==DEFAULT_DISPLAY_HEIGHT,"neutral dimensions invalid");
        runtime.graphics.pending=false;UploadFrame(texture,&runtime,width,height);
        require(uploads==2 && texture.bytes[0]==17 && runtime.graphics.copyCalls==1,"same-tick completion did not upload frame");
        UploadFrame(texture,&runtime,width,height);require(uploads==2,"same tick uploaded duplicate frame");
        runtime.scheduler.tick=1;runtime.graphics.pending=true;
        UploadFrame(texture,&runtime,width,height);require(uploads==2 && texture.bytes[0]==17,"pending next field replaced completed image");
        ++runtime.graphics.reset;
        UploadFrame(texture,&runtime,width,height);
        require(uploads==3 && neutralUploads==2 && texture.bytes[0]==0 && magentaImages==0,"reset pending producer retained prior-epoch pixels");
        UploadFrame(texture,&runtime,width,height);require(uploads==3,"pending reset repeatedly blanked texture");
        runtime.graphics.pending=false;runtime.graphics.value=34;
        UploadFrame(texture,&runtime,width,height);
        require(uploads==4 && texture.bytes[0]==34,"reset same-tick completion did not retry");
        std::cout<<"PASS exact UploadFrame: initial/reset neutral upload, retained image, same-tick retry, cache reuse\n";
        return 0;
    }
    catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
