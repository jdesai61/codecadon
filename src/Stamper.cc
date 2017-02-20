/* Copyright 2017 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <nan.h>
#include "Stamper.h"
#include "MyWorker.h"
#include "Timer.h"
#include "Memory.h"
#include "EssenceInfo.h"
#include "Packers.h"
#include "Primitives.h"
#include "Persist.h"

#include <memory>

using namespace v8;

namespace streampunk {

class WipeProcessData : public iProcessData {
public:
  WipeProcessData (Local<Object> dstBufObj, const iRect &wipeRect, const fCol &wipeCol)
    : mPersistentDstBuf(new Persist(dstBufObj)),
      mDstBuf(Memory::makeNew((uint8_t *)node::Buffer::Data(dstBufObj), (uint32_t)node::Buffer::Length(dstBufObj))),
      mWipeRect(wipeRect), mWipeCol(wipeCol)
  { }
  ~WipeProcessData() { }
  
  std::shared_ptr<Memory> dstBuf() const { return mDstBuf; }
  iRect wipeRect() const { return mWipeRect; }
  fCol wipeCol() const { return mWipeCol; }

private:
  std::unique_ptr<Persist> mPersistentDstBuf;
  std::shared_ptr<Memory> mDstBuf;
  iRect mWipeRect;
  fCol mWipeCol;
};

class CopyProcessData : public iProcessData {
public:
  CopyProcessData (Local<Object> srcBufObj, Local<Object> dstBufObj, const iXY &dstOrg)
    : mPersistentSrcBuf(new Persist(srcBufObj)),
      mPersistentDstBuf(new Persist(dstBufObj)),
      mSrcBuf(Memory::makeNew((uint8_t *)node::Buffer::Data(srcBufObj), (uint32_t)node::Buffer::Length(srcBufObj))),
      mDstBuf(Memory::makeNew((uint8_t *)node::Buffer::Data(dstBufObj), (uint32_t)node::Buffer::Length(dstBufObj))),
      mDstOrg(dstOrg)
  { }
  ~CopyProcessData() { }
  
  std::shared_ptr<Memory> srcBuf() const { return mSrcBuf; }
  std::shared_ptr<Memory> dstBuf() const { return mDstBuf; }
  iXY dstOrg() const { return mDstOrg; }

private:
  std::unique_ptr<Persist> mPersistentSrcBuf;
  std::unique_ptr<Persist> mPersistentDstBuf;
  std::shared_ptr<Memory> mSrcBuf;
  std::shared_ptr<Memory> mDstBuf;
  iXY mDstOrg;
};

Stamper::Stamper(Nan::Callback *callback) 
  : mWorker(new MyWorker(callback)), mSetInfoOK(false), mSrcFormatBytes(0), mDstBytesReq(0) {
  AsyncQueueWorker(mWorker);
}
Stamper::~Stamper() {}

// iProcess
uint32_t Stamper::processFrame (std::shared_ptr<iProcessData> processData) {
  Timer t;
  std::string func("stamp");
  std::shared_ptr<WipeProcessData> wpd = std::dynamic_pointer_cast<WipeProcessData>(processData);
  if (wpd) {
    func = "wipe";
    doWipe(wpd);
  }

  std::shared_ptr<CopyProcessData> cpd = std::dynamic_pointer_cast<CopyProcessData>(processData);
  if (cpd) {
    func = "copy";
    doCopy(cpd);
  }

  printf("%s: %.2fms\n", func.c_str(), t.delta());

  return mDstBytesReq;
}

void Stamper::doSetInfo(Local<Object> srcTags, Local<Object> dstTags) {
  mSrcVidInfo = std::make_shared<EssenceInfo>(srcTags); 
  printf ("Stamper SrcVidInfo: %s\n", mSrcVidInfo->toString().c_str());
  mDstVidInfo = std::make_shared<EssenceInfo>(dstTags); 
  printf("Stamper DstVidInfo: %s\n", mDstVidInfo->toString().c_str());

  if (mSrcVidInfo->packing().compare(mDstVidInfo->packing())) {
    std::string err = std::string("Source and destination format must be identical \'") + mSrcVidInfo->packing() + "\', \'" + mDstVidInfo->packing() + "\'";
    return Nan::ThrowError(err.c_str());
  }
  if (mSrcVidInfo->packing().compare("420P") && mSrcVidInfo->packing().compare("YUV422P10")) {
    std::string err = std::string("Unsupported source format \'") + mSrcVidInfo->packing() + "\'";
    return Nan::ThrowError(err.c_str());
  }
  if (mDstVidInfo->packing().compare("420P") && mDstVidInfo->packing().compare("YUV422P10")) { 
    std::string err = std::string("Unsupported destination packing type \'") + mDstVidInfo->packing() + "\'";
    Nan::ThrowError(err.c_str());
  }
  if ((mSrcVidInfo->width() % 2) || (mDstVidInfo->width() % 2)) {
    std::string err = std::string("Width must be divisible by 2 - src ") + std::to_string(mSrcVidInfo->width()) + ", dst " + std::to_string(mDstVidInfo->width());
    Nan::ThrowError(err.c_str());
  }

  mDstBytesReq = getFormatBytes(mDstVidInfo->packing(), mDstVidInfo->width(), mDstVidInfo->height());
}

void Stamper::doWipe(std::shared_ptr<WipeProcessData> wpd) {
  uint32_t blackLevel = 64;
  uint32_t lumaRange = 940 - blackLevel;
  uint32_t chromaRange = 960 - blackLevel;
  uint32_t chromaMid = 512;
  uint32_t bytesPerPixel = 2;
  uint32_t lumaLinesPerChromaLine = 1;
  if (0 == mSrcVidInfo->packing().compare("420P")) {
    blackLevel = 16;
    lumaRange = 235 - blackLevel;
    chromaRange = 240 - blackLevel;
    chromaMid = 128;
    bytesPerPixel = 1;
    lumaLinesPerChromaLine = 2;
  }

  iCol wipeCol (uint16_t(wpd->wipeCol().y * lumaRange + blackLevel),
                uint16_t(wpd->wipeCol().u * chromaRange + chromaMid),  
                uint16_t(wpd->wipeCol().v * chromaRange + chromaMid));

  uint32_t dstLumaPitchBytes = mDstVidInfo->width() * bytesPerPixel;
  uint32_t dstChromaPitchBytes = dstLumaPitchBytes / 2;
  uint32_t dstLumaPlaneBytes = dstLumaPitchBytes * mDstVidInfo->height();
  uint32_t dstChromaPlaneBytes = dstChromaPitchBytes * mDstVidInfo->height() / lumaLinesPerChromaLine;

  uint8_t *dstYLine = wpd->dstBuf()->buf();
  uint8_t *dstULine = dstYLine + dstLumaPlaneBytes;
  uint8_t *dstVLine = dstULine + dstChromaPlaneBytes;

  dstYLine += wpd->wipeRect().org.x * bytesPerPixel + dstLumaPitchBytes * wpd->wipeRect().org.y;
  dstULine += wpd->wipeRect().org.x * bytesPerPixel / 2 + dstChromaPitchBytes * wpd->wipeRect().org.y / lumaLinesPerChromaLine;
  dstVLine += wpd->wipeRect().org.x * bytesPerPixel / 2 + dstChromaPitchBytes * wpd->wipeRect().org.y / lumaLinesPerChromaLine;

  for (int32_t y=0; y<wpd->wipeRect().len.y; ++y) {
    bool evenLine = (y & 1) == 0;

    if (1==bytesPerPixel) {
      // 8-bit fill
      memset (dstYLine, wipeCol.y, wpd->wipeRect().len.x);
      memset (dstULine, wipeCol.u, wpd->wipeRect().len.x / 2);
      memset (dstVLine, wipeCol.v, wpd->wipeRect().len.x / 2);
    } else {
      // 10-bit fill
      uint32_t *dstY32Line = (uint32_t *)dstYLine;
      uint16_t *dstU16Line = (uint16_t *)dstULine;
      uint16_t *dstV16Line = (uint16_t *)dstVLine;

      for (int32_t x=0; x < wpd->wipeRect().len.x / 2; ++x) { 
        *dstY32Line++ = (wipeCol.y << 16) | wipeCol.y;
        *dstU16Line++ = wipeCol.u;
        *dstV16Line++ = wipeCol.v;
      }
    }

    dstYLine += dstLumaPitchBytes; 
    if ((1 == lumaLinesPerChromaLine) || !evenLine) {
      dstULine += dstChromaPitchBytes;
      dstVLine += dstChromaPitchBytes;
    } 
  }
}

void Stamper::doCopy(std::shared_ptr<CopyProcessData> cpd) {
  uint32_t bytesPerPixel = 2;
  uint32_t lumaLinesPerChromaLine = 1;
  if (0 == mSrcVidInfo->packing().compare("420P")) {
    bytesPerPixel = 1;
    lumaLinesPerChromaLine = 2;
  }

  uint32_t srcLumaPitchBytes = mSrcVidInfo->width() * bytesPerPixel;
  uint32_t srcChromaPitchBytes = srcLumaPitchBytes / 2;
  uint32_t srcLumaPlaneBytes = srcLumaPitchBytes * mSrcVidInfo->height();
  uint32_t srcChromaPlaneBytes = srcChromaPitchBytes * mSrcVidInfo->height() / lumaLinesPerChromaLine;

  uint32_t dstLumaPitchBytes = mDstVidInfo->width() * bytesPerPixel;
  uint32_t dstChromaPitchBytes = dstLumaPitchBytes / 2;
  uint32_t dstLumaPlaneBytes = dstLumaPitchBytes * mDstVidInfo->height();
  uint32_t dstChromaPlaneBytes = dstChromaPitchBytes * mDstVidInfo->height() / lumaLinesPerChromaLine;

  const uint8_t *srcYLine = cpd->srcBuf()->buf();
  const uint8_t *srcULine = srcYLine + srcLumaPlaneBytes;
  const uint8_t *srcVLine = srcULine + srcChromaPlaneBytes;

  uint8_t *dstYLine = cpd->dstBuf()->buf();
  uint8_t *dstULine = dstYLine + dstLumaPlaneBytes;
  uint8_t *dstVLine = dstULine + dstChromaPlaneBytes;

  dstYLine += cpd->dstOrg().x * bytesPerPixel + dstLumaPitchBytes * cpd->dstOrg().y;
  dstULine += cpd->dstOrg().x * bytesPerPixel / 2 + dstChromaPitchBytes * cpd->dstOrg().y / lumaLinesPerChromaLine;
  dstVLine += cpd->dstOrg().x * bytesPerPixel / 2 + dstChromaPitchBytes * cpd->dstOrg().y / lumaLinesPerChromaLine;

  for (uint32_t y=0; y<mSrcVidInfo->height(); ++y) {
    bool evenLine = (y & 1) == 0;

    memcpy(dstYLine, srcYLine, srcLumaPitchBytes);
    memcpy(dstULine, srcULine, srcChromaPitchBytes);
    memcpy(dstVLine, srcVLine, srcChromaPitchBytes);

    srcYLine += srcLumaPitchBytes;
    dstYLine += dstLumaPitchBytes; 
    if ((1 == lumaLinesPerChromaLine) || !evenLine) {
      srcULine += srcChromaPitchBytes;
      srcVLine += srcChromaPitchBytes;
      dstULine += dstChromaPitchBytes;
      dstVLine += dstChromaPitchBytes;
    } 
  }
}

NAN_METHOD(Stamper::SetInfo) {
  if (info.Length() != 2)
    return Nan::ThrowError("Stamper SetInfo expects 2 arguments");
  Local<Object> srcTags = Local<Object>::Cast(info[0]);
  Local<Object> dstTags = Local<Object>::Cast(info[1]);

  Stamper* obj = Nan::ObjectWrap::Unwrap<Stamper>(info.Holder());

  Nan::TryCatch try_catch;
  obj->doSetInfo(srcTags, dstTags);
  if (try_catch.HasCaught()) {
    obj->mSetInfoOK = false;
    try_catch.ReThrow();
    return;
  }

  obj->mSetInfoOK = true;
  info.GetReturnValue().Set(Nan::New(obj->mDstBytesReq));
}

NAN_METHOD(Stamper::Wipe) {
  if (info.Length() != 3)
    return Nan::ThrowError("Stamper Wipe expects 3 arguments");
  if (!info[0]->IsObject())
    return Nan::ThrowError("Stamper Wipe requires a valid destination buffer as the first parameter");
  if (!info[1]->IsObject())
    return Nan::ThrowError("Stamper Wipe requires a valid params object as the second parameter");
  if (!info[2]->IsFunction())
    return Nan::ThrowError("Stamper Wipe requires a valid callback as the third parameter");

  Local<Object> dstBufObj = Local<Object>::Cast(info[0]);
  Local<Object> paramTags = Local<Object>::Cast(info[1]);
  Local<Function> callback = Local<Function>::Cast(info[2]);

  Stamper* obj = Nan::ObjectWrap::Unwrap<Stamper>(info.Holder());

  if (!obj->mSetInfoOK)
    return Nan::ThrowError("Wipe called with incorrect setup parameters");

  if (obj->mDstBytesReq > node::Buffer::Length(dstBufObj))
    return Nan::ThrowError("Insufficient destination buffer for specified format");

  Local<String> wipeRectStr = Nan::New<String>("wipeRect").ToLocalChecked();
  Local<Array> wipeRectArr = Local<Array>::Cast(Nan::Get(paramTags, wipeRectStr).ToLocalChecked());
  if (!(!wipeRectArr->IsNull() && wipeRectArr->IsArray() && (wipeRectArr->Length() == 4)))
    return Nan::ThrowError("wipeRect parameter invalid");
  iRect wipeRect(iXY(Nan::To<uint32_t>(wipeRectArr->Get(0)).FromJust(), Nan::To<uint32_t>(wipeRectArr->Get(1)).FromJust()),
                 iXY(Nan::To<uint32_t>(wipeRectArr->Get(2)).FromJust(), Nan::To<uint32_t>(wipeRectArr->Get(3)).FromJust()));

  Local<String> wipeColStr = Nan::New<String>("wipeCol").ToLocalChecked();
  Local<Array> wipeColArr = Local<Array>::Cast(Nan::Get(paramTags, wipeColStr).ToLocalChecked());
  if (!(!wipeColArr->IsNull() && wipeColArr->IsArray() && (wipeColArr->Length() == 3)))
    return Nan::ThrowError("wipeCol parameter invalid");

  fCol wipeCol(Nan::To<double>(wipeColArr->Get(0)).FromJust(), 
               Nan::To<double>(wipeColArr->Get(1)).FromJust(),
               Nan::To<double>(wipeColArr->Get(2)).FromJust());

  std::shared_ptr<iProcessData> wpd = 
    std::make_shared<WipeProcessData>(dstBufObj, wipeRect, wipeCol);
  obj->mWorker->doFrame(wpd, obj, new Nan::Callback(callback));

  info.GetReturnValue().Set(Nan::New(obj->mWorker->numQueued()));
}

NAN_METHOD(Stamper::Copy) {
  if (info.Length() != 4)
    return Nan::ThrowError("Stamper Copy expects 4 arguments");
  if (!info[0]->IsArray())
    return Nan::ThrowError("Stamper Copy requires a valid source buffer array as the first parameter");
  if (!info[1]->IsObject())
    return Nan::ThrowError("Stamper Copy requires a valid destination buffer as the second parameter");
  if (!info[2]->IsObject())
    return Nan::ThrowError("Stamper Copy requires a valid params object as the third parameter");
  if (!info[3]->IsFunction())
    return Nan::ThrowError("Stamper Copy requires a valid callback as the fourth parameter");

  Local<Array> srcBufArray = Local<Array>::Cast(info[0]);
  Local<Object> dstBufObj = Local<Object>::Cast(info[1]);
  Local<Object> paramTags = Local<Object>::Cast(info[2]);
  Local<Function> callback = Local<Function>::Cast(info[3]);

  Local<Object> srcBufObj = Local<Object>::Cast(srcBufArray->Get(0));

  Stamper* obj = Nan::ObjectWrap::Unwrap<Stamper>(info.Holder());

  if (!obj->mSetInfoOK)
    return Nan::ThrowError("Copy called with incorrect setup parameters");

  obj->mSrcFormatBytes = getFormatBytes(obj->mSrcVidInfo->packing(), obj->mSrcVidInfo->width(), obj->mSrcVidInfo->height());
  if (obj->mSrcFormatBytes > (uint32_t)node::Buffer::Length(srcBufObj))
    Nan::ThrowError("Insufficient source buffer for Copy\n");

  if (obj->mDstBytesReq > node::Buffer::Length(dstBufObj))
    return Nan::ThrowError("Insufficient destination buffer for specified format");

  Local<String> dstOrgStr = Nan::New<String>("dstOrg").ToLocalChecked();
  Local<Array> dstOrgXY = Local<Array>::Cast(Nan::Get(paramTags, dstOrgStr).ToLocalChecked());
  if (!(!dstOrgXY->IsNull() && dstOrgXY->IsArray() && (dstOrgXY->Length() == 2)))
    return Nan::ThrowError("dstOrg parameter invalid");
  iXY dstOrg(Nan::To<uint32_t>(dstOrgXY->Get(0)).FromJust(), Nan::To<uint32_t>(dstOrgXY->Get(1)).FromJust());

  std::shared_ptr<iProcessData> cpd = 
    std::make_shared<CopyProcessData>(srcBufObj, dstBufObj, dstOrg);
  obj->mWorker->doFrame(cpd, obj, new Nan::Callback(callback));

  info.GetReturnValue().Set(Nan::New(obj->mWorker->numQueued()));
}

NAN_METHOD(Stamper::Quit) {
  if (info.Length() != 1)
    return Nan::ThrowError("Packer quit expects 1 argument");
  if (!info[0]->IsFunction())
    return Nan::ThrowError("Packer quit requires a valid callback as the parameter");
  Nan::Callback *callback = new Nan::Callback(Local<Function>::Cast(info[0]));
  Stamper* obj = Nan::ObjectWrap::Unwrap<Stamper>(info.Holder());

  if (obj->mWorker != NULL)
    obj->mWorker->quit(callback);

  info.GetReturnValue().SetUndefined();
}

NAN_MODULE_INIT(Stamper::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("Copy").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  SetPrototypeMethod(tpl, "setInfo", SetInfo);
  SetPrototypeMethod(tpl, "wipe", Wipe);
  SetPrototypeMethod(tpl, "copy", Copy);
  SetPrototypeMethod(tpl, "quit", Quit);

  constructor().Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, Nan::New("Stamper").ToLocalChecked(),
    Nan::GetFunction(tpl).ToLocalChecked());
}

} // namespace streampunk
