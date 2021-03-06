/*
 * Copyright 2007 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkImageEncoderPriv.h"

#ifdef SK_HAS_JPEG_LIBRARY

#include "SkColorPriv.h"
#include "SkImageEncoderFns.h"
#include "SkImageInfoPriv.h"
#include "SkJpegEncoder.h"
#include "SkJPEGWriteUtility.h"
#include "SkStream.h"
#include "SkTemplates.h"

#include <stdio.h>

extern "C" {
    #include "jpeglib.h"
    #include "jerror.h"
}

// This warning triggers false postives way too often in here.
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic ignored "-Wclobbered"
#endif

class SkJpegEncoderMgr : SkNoncopyable {
public:

    /*
     * Create the decode manager
     * Does not take ownership of stream
     */
    static std::unique_ptr<SkJpegEncoderMgr> Make(SkWStream* stream) {
        return std::unique_ptr<SkJpegEncoderMgr>(new SkJpegEncoderMgr(stream));
    }

    bool setParams(const SkImageInfo& srcInfo);

    jpeg_compress_struct* cinfo() { return &fCInfo; }

    jmp_buf& jmpBuf() { return fErrMgr.fJmpBuf; }

    transform_scanline_proc proc() const { return fProc; }

    ~SkJpegEncoderMgr() {
        jpeg_destroy_compress(&fCInfo);
    }

private:

    SkJpegEncoderMgr(SkWStream* stream)
        : fDstMgr(stream)
        , fProc(nullptr)
    {
        fCInfo.err = jpeg_std_error(&fErrMgr);
        fErrMgr.error_exit = skjpeg_error_exit;
        jpeg_create_compress(&fCInfo);
        fCInfo.dest = &fDstMgr;
    }

    jpeg_compress_struct    fCInfo;
    skjpeg_error_mgr        fErrMgr;
    skjpeg_destination_mgr  fDstMgr;
    transform_scanline_proc fProc;
};

bool SkJpegEncoderMgr::setParams(const SkImageInfo& srcInfo) {
    J_COLOR_SPACE jpegColorType = JCS_EXT_RGBA;
    int numComponents = 0;
    switch (srcInfo.colorType()) {
        case kRGBA_8888_SkColorType:
            jpegColorType = JCS_EXT_RGBA;
            numComponents = 4;
            break;
        case kBGRA_8888_SkColorType:
            jpegColorType = JCS_EXT_BGRA;
            numComponents = 4;
            break;
        case kRGB_565_SkColorType:
            fProc = transform_scanline_565;
            jpegColorType = JCS_RGB;
            numComponents = 3;
            break;
        case kARGB_4444_SkColorType:
            fProc = transform_scanline_444;
            jpegColorType = JCS_RGB;
            numComponents = 3;
            break;
        case kIndex_8_SkColorType:
            fProc = transform_scanline_index8_opaque;
            jpegColorType = JCS_RGB;
            numComponents = 3;
            break;
        case kGray_8_SkColorType:
            SkASSERT(srcInfo.isOpaque());
            jpegColorType = JCS_GRAYSCALE;
            numComponents = 1;
            break;
        case kRGBA_F16_SkColorType:
            if (!srcInfo.colorSpace() || !srcInfo.colorSpace()->gammaIsLinear()) {
                return false;
            }

            fProc = transform_scanline_F16_to_8888;
            jpegColorType = JCS_EXT_RGBA;
            numComponents = 4;
            break;
        default:
            return false;
    }

    fCInfo.image_width = srcInfo.width();
    fCInfo.image_height = srcInfo.height();
    fCInfo.in_color_space = jpegColorType;
    fCInfo.input_components = numComponents;
    jpeg_set_defaults(&fCInfo);

    // Tells libjpeg-turbo to compute optimal Huffman coding tables
    // for the image.  This improves compression at the cost of
    // slower encode performance.
    fCInfo.optimize_coding = TRUE;
    return true;
}

class SkJpegEncoder_Base : public SkJpegEncoder {
public:
    SkJpegEncoder_Base(std::unique_ptr<SkJpegEncoderMgr> encoderMgr, const SkPixmap& src);

    bool onEncodeRows(int numRows);

private:
    std::unique_ptr<SkJpegEncoderMgr> fEncoderMgr;
    SkPixmap                          fSrc;
    int                               fCurrRow;
    SkAutoTMalloc<uint8_t>            fStorage;
};

std::unique_ptr<SkJpegEncoder> SkJpegEncoder::Make(SkWStream* dst, const SkPixmap& src,
                                                   const Options& options) {
    if (!SkImageInfoIsValidAllowNumericalCS(src.info()) || !src.addr() ||
            src.rowBytes() < src.info().minRowBytes()) {
        return nullptr;
    }

    std::unique_ptr<SkJpegEncoderMgr> encoderMgr = SkJpegEncoderMgr::Make(dst);
    if (setjmp(encoderMgr->jmpBuf())) {
        return nullptr;
    }

    if (!encoderMgr->setParams(src.info())) {
        return nullptr;
    }

    jpeg_set_quality(encoderMgr->cinfo(), options.fQuality, TRUE);
    jpeg_start_compress(encoderMgr->cinfo(), TRUE);

    if (src.colorSpace()) {
        sk_sp<SkData> icc = icc_from_color_space(*src.colorSpace());
        if (icc) {
            // Create a contiguous block of memory with the icc signature followed by the profile.
            sk_sp<SkData> markerData =
                    SkData::MakeUninitialized(kICCMarkerHeaderSize + icc->size());
            uint8_t* ptr = (uint8_t*) markerData->writable_data();
            memcpy(ptr, kICCSig, sizeof(kICCSig));
            ptr += sizeof(kICCSig);
            *ptr++ = 1; // This is the first marker.
            *ptr++ = 1; // Out of one total markers.
            memcpy(ptr, icc->data(), icc->size());

            jpeg_write_marker(encoderMgr->cinfo(), kICCMarker, markerData->bytes(),
                              markerData->size());
        }
    }

    return std::unique_ptr<SkJpegEncoder>(new SkJpegEncoder_Base(std::move(encoderMgr), src));
}


SkJpegEncoder_Base::SkJpegEncoder_Base(std::unique_ptr<SkJpegEncoderMgr> encoderMgr,
                                       const SkPixmap& src)
    : fEncoderMgr(std::move(encoderMgr))
    , fSrc(src)
    , fCurrRow(0)
    , fStorage(fEncoderMgr->proc() ? fEncoderMgr->cinfo()->input_components*src.width() : 0)
{}

bool SkJpegEncoder::encodeRows(int numRows) {
    return ((SkJpegEncoder_Base*) this)->onEncodeRows(numRows);
}

bool SkJpegEncoder_Base::onEncodeRows(int numRows) {
    SkASSERT(numRows > 0 && fCurrRow < fSrc.height());
    if (numRows <= 0 || fCurrRow >= fSrc.height()) {
        return false;
    }

    if (fCurrRow + numRows > fSrc.height()) {
        numRows = fSrc.height() - fCurrRow;
    }

    if (setjmp(fEncoderMgr->jmpBuf())) {
        // Short circuit any future calls after failing.
        fCurrRow = fSrc.height();
        return false;
    }

    const void* srcRow = fSrc.addr(0, fCurrRow);
    const SkPMColor* colors = fSrc.ctable() ? fSrc.ctable()->readColors() : nullptr;
    for (int i = 0; i < numRows; i++) {
        JSAMPLE* jpegSrcRow = (JSAMPLE*) srcRow;
        if (fEncoderMgr->proc()) {
            fEncoderMgr->proc()((char*)fStorage.get(), (const char*)srcRow, fSrc.width(),
                                fEncoderMgr->cinfo()->input_components, colors);
            jpegSrcRow = fStorage.get();
        }

        jpeg_write_scanlines(fEncoderMgr->cinfo(), &jpegSrcRow, 1);
        srcRow = SkTAddOffset<const void>(srcRow, fSrc.rowBytes());
    }

    fCurrRow += numRows;
    if (fCurrRow == fSrc.height()) {
        jpeg_finish_compress(fEncoderMgr->cinfo());
    }

    return true;
}

bool SkJpegEncoder::Encode(SkWStream* dst, const SkPixmap& src, const Options& options) {
    auto encoder = SkJpegEncoder::Make(dst, src, options);
    return encoder.get() && encoder->encodeRows(src.height());
}

#endif
