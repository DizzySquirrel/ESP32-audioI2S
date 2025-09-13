/*
 * flac_decoder.cpp
 * Java source code from https://www.nayuki.io/page/simple-flac-implementation
 * adapted to ESP32
 *
 * Created on: Jul 03,2020
 * Updated on: Aug 28,2025
 *
 * Author: Wolle
 *
 */
#include "flac_decoder.h"

//----------------------------------------------------------------------------------------------------------------------
//          FLAC INI SECTION
//----------------------------------------------------------------------------------------------------------------------

bool FlacDecoder::init() {
    FLACFrameHeader.alloc();
    FLACMetadataBlock.alloc();

    m_samplesBuffer.resize(FLAC_MAX_CHANNELS);
    for (int32_t i = 0; i < FLAC_MAX_CHANNELS; i++){
        m_samplesBuffer[i].alloc(m_maxBlocksize  * sizeof(int32_t));
        if(!m_samplesBuffer[i]){ // ps_ptr<T> sollte operator bool() überladen
            FLAC_LOG_ERROR("not enough memory to allocate flacdecoder buffers");
            m_samplesBuffer.clear(); // Vektor leeren und alle ps_ptr Objekte zerstören
            return false;
        }
    }

    clear();
    setDefaults();
    m_flacPageNr = 0;
    return true;
}
//----------------------------------------------------------------------------------------------------------------------
void FlacDecoder::clear(){
    FLACFrameHeader.zero_mem();
    FLACMetadataBlock.zero_mem();

    for (int32_t i = 0; i < FLAC_MAX_CHANNELS; i++){
        m_samplesBuffer[i].zero_mem();
    }

    m_flacSegmTableVec.clear(); m_flacSegmTableVec.shrink_to_fit();
    m_flacStatus = DECODE_FRAME;
    return;
}
//----------------------------------------------------------------------------------------------------------------------
void FlacDecoder::reset(){
    FLACFrameHeader.reset();
    FLACMetadataBlock.reset();
    m_flacStreamTitle.reset();
    m_flacVendorString.reset();

    m_samplesBuffer.clear(); m_samplesBuffer.shrink_to_fit();
    coefs.clear(); coefs.shrink_to_fit();
    m_flacSegmTableVec.clear(); m_flacSegmTableVec.shrink_to_fit();
    m_flacBlockPicItem.clear(); m_flacBlockPicItem.shrink_to_fit();
}
//----------------------------------------------------------------------------------------------------------------------
void FlacDecoder::setDefaults(){
    coefs.clear(); coefs.shrink_to_fit();
    m_flacSegmTableVec.clear(); m_flacSegmTableVec.shrink_to_fit();
    m_flacBlockPicItem.clear(); m_flacBlockPicItem.shrink_to_fit();
    m_flac_bitBuffer = 0;
    m_flacBitrate = 0;
    m_flacBlockPicLenUntilFrameEnd = 0;
    m_flacCurrentFilePos = 0;
    m_flacBlockPicPos = 0;
    m_flacBlockPicLen = 0;
    m_flacRemainBlockPicLen = 0;
    m_flacAudioDataStart = 0;
    m_numOfOutSamples = 0;
    m_offset = 0;
    m_flacValidSamples = 0;
    m_rIndex = 0;
    m_flacStatus = DECODE_FRAME;
    m_flacCompressionRatio = 0;
    m_flacBitBufferLen = 0;
    m_flac_pageSegments = 0;
    m_f_flacNewStreamtitle = false;
    m_f_flacFirstCall = true;
    m_f_oggWrapper = false;
    m_f_lastMetaDataBlock = false;
    m_f_flacNewMetadataBlockPicture = false;
    m_f_flacParseOgg = false;
    m_f_bitReaderError = false;
    m_nBytes = 0;
}
//----------------------------------------------------------------------------------------------------------------------
//            B I T R E A D E R
//----------------------------------------------------------------------------------------------------------------------

uint32_t FlacDecoder::readUint(uint8_t nBits, int32_t *bytesLeft){

const uint32_t mask[33] = {0x00000000, 0x00000001, 0x00000003, 0x00000007, 0x0000000f, 0x0000001f, 0x0000003f,
                         0x0000007f, 0x000000ff, 0x000001ff, 0x000003ff, 0x000007ff, 0x00000fff, 0x00001fff,
                         0x00003fff, 0x00007fff, 0x0000ffff, 0x0001ffff, 0x0003ffff, 0x0007ffff, 0x000fffff,
                         0x001fffff, 0x003fffff, 0x007fffff, 0x00ffffff, 0x01ffffff, 0x03ffffff, 0x07ffffff,
                         0x0fffffff, 0x1fffffff, 0x3fffffff, 0x7fffffff, 0xffffffff};


    while (m_flacBitBufferLen < nBits){
        uint8_t temp = *(m_flacInptr + m_rIndex);
        m_rIndex++;
        (*bytesLeft)--;
        if(*bytesLeft < 0) { FLAC_LOG_ERROR("error in bitreader"); m_f_bitReaderError = true; break;}
        m_flac_bitBuffer = (m_flac_bitBuffer << 8) | temp;
        m_flacBitBufferLen += 8;
    }
    m_flacBitBufferLen -= nBits;
    uint32_t result = m_flac_bitBuffer >> m_flacBitBufferLen;
    if (nBits < 32)
        result &= mask[nBits];
    return result;
}

int32_t FlacDecoder::readSignedInt(int32_t nBits, int32_t* bytesLeft){
    int32_t temp = readUint(nBits, bytesLeft) << (32 - nBits);
    temp = temp >> (32 - nBits); // The C++ compiler uses the sign bit to fill vacated bit positions
    return temp;
}

int64_t FlacDecoder::readRiceSignedInt(uint8_t param, int32_t* bytesLeft){
    long val = 0;
    while (readUint(1, bytesLeft) == 0)
        val++;
    val = (val << param) | readUint(param, bytesLeft);
    return (val >> 1) ^ -(val & 1);
}

void FlacDecoder::alignToByte() {
    m_flacBitBufferLen -= m_flacBitBufferLen % 8;
}
//----------------------------------------------------------------------------------------------------------------------
//              F L A C - D E C O D E R
//----------------------------------------------------------------------------------------------------------------------
void FlacDecoder::setRawBlockParams(uint8_t channels, uint32_t sampleRate, uint8_t BPS, uint32_t tsis, uint32_t AuDaLength){
log_w("channels %i, sampleRate %i, BPS %i, tsis %i, AuDaLength %i", channels, sampleRate, BPS, tsis, AuDaLength);
    FLACMetadataBlock->numChannels = channels;
    FLACMetadataBlock->sampleRate = sampleRate;
    FLACMetadataBlock->bitsPerSample = BPS;
    FLACMetadataBlock->totalSamples = tsis;  // total samples in stream
    FLACMetadataBlock->audioDataLength = AuDaLength;
}
//----------------------------------------------------------------------------------------------------------------------
void FlacDecoder::decoderReset(){ // set var to default
    setDefaults();
    clear();
}
//----------------------------------------------------------------------------------------------------------------------
int32_t FlacDecoder::findSyncWord(uint8_t *buf, int32_t nBytes) {

    int32_t i = specialIndexOf(buf, "OggS", nBytes);
    if(i == 0) {m_f_bitReaderError = false; return 0;}  // flag has ogg wrapper

    if(m_f_oggWrapper && i > 0){
        m_f_bitReaderError = false;
        return i;
    }
    else{
         /* find byte-aligned sync code - need 14 matching bits */
        for (i = 0; i < nBytes - 1; i++) {
            if ((buf[i + 0] & 0xFF) == 0xFF  && (buf[i + 1] & 0xFC) == 0xF8) { // <14> Sync code '11111111111110xx'
                if(i) decoderReset();
            //    m_f_bitReaderError = false;
                return i;
            }
        }
    }
    return -1;
}
//----------------------------------------------------------------------------------------------------------------------
boolean FlacDecoder::FLACFindMagicWord(unsigned char* buf, int32_t nBytes){
    int32_t idx = specialIndexOf(buf, "fLaC", nBytes);
    if(idx >0){ // Metadatablock follows
        idx += 4;
        boolean lmdbf = ((buf[idx + 1] & 0x80) == 0x80); // Last-metadata-block flag
        uint8_t bt = (buf[idx + 1] & 0x7F); // block type
        uint32_t lomd = (buf[idx + 2] << 16) + (buf[idx + 3] << 8) + buf[idx + 4]; // Length of metadata to follow

        (void)lmdbf; (void)bt; (void)lomd;
        // FLAC_LOG_INFO("Last-metadata-block flag: %d", lmdbf);
        // FLAC_LOG_INFO("block type: %d", bt);
        // FLAC_LOG_INFO("Length (in bytes) of metadata to follow: %d", lomd);
        return true;
    }
    return false;
}
//----------------------------------------------------------------------------------------------------------------------
const char* FlacDecoder::getStreamTitle(){
    if(m_f_flacNewStreamtitle){
        m_f_flacNewStreamtitle = false;
        return m_flacStreamTitle.get();
    }
    return NULL;
}
//----------------------------------------------------------------------------------------------------------------------
int32_t FlacDecoder::parseOGG(uint8_t *inbuf, int32_t *bytesLeft){  // reference https://www.xiph.org/ogg/doc/rfc3533.txt

    m_f_flacParseOgg = false;
    int32_t idx = specialIndexOf(inbuf, "OggS", 6);
    if(idx != 0){FLAC_LOG_ERROR("Flac decoder asyncron, \"OggS\" not found"); return FLAC_ERR;}

    uint8_t  version            = *(inbuf +  4); (void) version;
    uint8_t  headerType         = *(inbuf +  5); (void) headerType;
    uint64_t granulePosition    = (uint64_t)*(inbuf + 13) << 56;  // granule_position: an 8 Byte field containing -
             granulePosition   += (uint64_t)*(inbuf + 12) << 48;  // position information. For an audio stream, it MAY
             granulePosition   += (uint64_t)*(inbuf + 11) << 40;  // contain the total number of PCM samples encoded
             granulePosition   += (uint64_t)*(inbuf + 10) << 32;  // after including all frames finished on this page.
             granulePosition   += *(inbuf +  9) << 24;  // This is a hint for the decoder and gives it some timing
             granulePosition   += *(inbuf +  8) << 16;  // and position information. A special value of -1 (in two's
             granulePosition   += *(inbuf +  7) << 8;   // complement) indicates that no packets finish on this page.
             granulePosition   += *(inbuf +  6); (void) granulePosition;
    uint32_t bitstreamSerialNr  = *(inbuf + 17) << 24;  // bitstream_serial_number: a 4 Byte field containing the
             bitstreamSerialNr += *(inbuf + 16) << 16;  // unique serial number by which the logical bitstream
             bitstreamSerialNr += *(inbuf + 15) << 8;   // is identified.
             bitstreamSerialNr += *(inbuf + 14); (void) bitstreamSerialNr;
    uint32_t pageSequenceNr     = *(inbuf + 21) << 24;  // page_sequence_number: a 4 Byte field containing the sequence
             pageSequenceNr    += *(inbuf + 20) << 16;  // number of the page so the decoder can identify page loss
             pageSequenceNr    += *(inbuf + 19) << 8;   // This sequence number is increasing on each logical bitstream
             pageSequenceNr    += *(inbuf + 18); (void) pageSequenceNr;
    uint32_t CRCchecksum        = *(inbuf + 25) << 24;
             CRCchecksum       += *(inbuf + 24) << 16;
             CRCchecksum       += *(inbuf + 23) << 8;
             CRCchecksum       += *(inbuf + 22); (void) CRCchecksum;
    uint8_t  pageSegments       = *(inbuf + 26);        // giving the number of segment entries

    // read the segment table (contains pageSegments bytes),  1...251: Length of the frame in bytes,
    // 255: A second byte is needed.  The total length is first_byte + second byte
    m_flacSegmTableVec.clear();
    m_flacSegmTableVec.shrink_to_fit();
    for(int32_t i = 0; i < pageSegments; i++){
        int32_t n = *(inbuf + 27 + i);
        while(*(inbuf + 27 + i) == 255){
            i++;
            if(i == pageSegments) break;
            n+= *(inbuf + 27 + i);
        }
        m_flacSegmTableVec.insert(m_flacSegmTableVec.begin(), n);
    }
    // for(int32_t i = 0; i< m_flacSegmTableVec.size(); i++){FLAC_LOG_INFO("%i", m_flacSegmTableVec[i]);}

    bool     continuedPage = headerType & 0x01; // set: page contains data of a packet continued from the previous page
    bool     firstPage     = headerType & 0x02; // set: this is the first page of a logical bitstream (bos)
    bool     lastPage      = headerType & 0x04; // set: this is the last page of a logical bitstream (eos)

    (void)continuedPage; (void)lastPage;

    // FLAC_LOG_INFO("firstPage %i, continuedPage %i, lastPage %i", firstPage, continuedPage, lastPage);

    if(firstPage) m_flacPageNr = 0;

    uint32_t headerSize = pageSegments + 27;

    *bytesLeft -= headerSize;
    m_flacCurrentFilePos += headerSize;
    return FLAC_NONE; // no error
}

//----------------------------------------------------------------------------------------------------------------------------------------------------
std::vector<uint32_t> FlacDecoder::getMetadataBlockPicture(){
    if(m_f_flacNewMetadataBlockPicture){
        m_f_flacNewMetadataBlockPicture = false;
        return m_flacBlockPicItem;
    }
    if(m_flacBlockPicItem.size() > 0){
        m_flacBlockPicItem.clear();
        m_flacBlockPicItem.shrink_to_fit();
    }
    return m_flacBlockPicItem;
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
int32_t FlacDecoder::parseFlacFirstPacket(uint8_t *inbuf, int16_t nBytes){ // 4.2.2. Identification header   https://xiph.org/flac/ogg_mapping.html

    int32_t ret = 0;
    int32_t idx = specialIndexOf(inbuf, "fLaC", nBytes);
    //FLAC_LOG_INFO("idx %i, nBytes %i", idx, nBytes);
    if(idx >= 0){ // FLAC signature found
        ret = idx + 4;
    }
    else {
        FLAC_LOG_ERROR("Flac signature \"fLaC\" not found");
        ret = FLAC_ERR;
    }
    return ret;
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
int32_t FlacDecoder::parseMetaDataBlockHeader(uint8_t *inbuf, int16_t nBytes){
    int8_t   ret = FLAC_PARSE_OGG_DONE;
    uint16_t pos = 0;
    int32_t  blockLength = 0;
    uint16_t minBlocksize = 0;
    uint16_t maxBlocksize = 0;
    uint32_t minFrameSize = 0;
    uint32_t maxFrameSize = 0;
    uint32_t sampleRate = 0;
    uint32_t vendorLength = 0;
    uint32_t commemtStringLength = 0;
    uint32_t userCommentListLength = 0;
    uint8_t  nrOfChannels = 0;
    uint8_t  bitsPerSample = 0;
    uint64_t totalSamplesInStream = 0;
    uint8_t  mdBlockHeader = 0;
    uint8_t  blockType = 0;
    uint8_t  bt = 0;
    std::vector<ps_ptr<char>> vb(8); // vorbis comment

    enum {streamInfo, padding, application, seekTable, vorbisComment, cueSheet, picture};


    while(true){
        mdBlockHeader         = *(inbuf + pos);
        m_f_lastMetaDataBlock = mdBlockHeader & 0b10000000; // FLAC_LOG_INFO("lastMdBlockFlag %i", m_f_lastMetaDataBlock);
        blockType             = mdBlockHeader & 0b01111111; // FLAC_LOG_INFO("blockType %i", blockType);

        blockLength        = *(inbuf + pos + 1) << 16;
        blockLength       += *(inbuf + pos + 2) << 8;
        blockLength       += *(inbuf + pos + 3); // FLAC_LOG_INFO("blockLength %i", blockLength);

        nBytes -= 4;
        pos += 4;

        switch(blockType) {
            case 0:
                bt = streamInfo;
                break;
            case 1:
                bt = padding;
            //  FLAC_LOG_ERROR("padding");
                return FLAC_NONE;
                break;
            case 2:
                bt = application;
                FLAC_LOG_ERROR("Flac unimplemented block type: %i", blockType);
                return FLAC_ERR;
                break;
            case 3:
                bt = seekTable;
                FLAC_LOG_ERROR("Flac unimplemented seek table: %i", seekTable);
                return FLAC_ERR;
                break;
            case 4:
                bt = vorbisComment;
                break;
            case 5:
                bt = cueSheet;
                FLAC_LOG_ERROR("Flac unimplemented cue sheet: %i", cueSheet);
                return FLAC_ERR;
                break;
            case 6:
                bt = picture;
                break;
            default:
                bt = streamInfo;
                //return ERR_FLAC_UNIMPLEMENTED;
                break;
        }

        switch(bt){
            case streamInfo:
                minBlocksize += *(inbuf + pos + 0) << 8;
                minBlocksize += *(inbuf + pos + 1);
                maxBlocksize += *(inbuf + pos + 2) << 8;
                maxBlocksize += *(inbuf + pos + 3);
                //FLAC_LOG_INFO("minBlocksize %i", minBlocksize);
                //FLAC_LOG_INFO("maxBlocksize %i", maxBlocksize);
                FLACMetadataBlock->minblocksize = minBlocksize;
                FLACMetadataBlock->maxblocksize = maxBlocksize;

                if(maxBlocksize > m_maxBlocksize){FLAC_LOG_ERROR("s_blocksize is too big: %i bytes, max block size: %i", maxBlocksize, m_maxBlocksize); return FLAC_ERR;}

                minFrameSize  = *(inbuf + pos + 4) << 16;
                minFrameSize += *(inbuf + pos + 5) << 8;
                minFrameSize += *(inbuf + pos + 6);
                maxFrameSize  = *(inbuf + pos + 7) << 16;
                maxFrameSize += *(inbuf + pos + 8) << 8;
                maxFrameSize += *(inbuf + pos + 9);
                //FLAC_LOG_INFO("minFrameSize %i", minFrameSize);
                //FLAC_LOG_INFO("maxFrameSize %i", maxFrameSize);
                FLACMetadataBlock->minframesize = minFrameSize;
                FLACMetadataBlock->maxframesize = maxFrameSize;

                sampleRate   =  *(inbuf + pos + 10) << 12;
                sampleRate  +=  *(inbuf + pos + 11) << 4;
                sampleRate  += (*(inbuf + pos + 12) & 0xF0) >> 4;
                //FLAC_LOG_INFO("sampleRate %i", sampleRate);
                FLACMetadataBlock->sampleRate = sampleRate;

                nrOfChannels = ((*(inbuf + pos + 12) & 0x0E) >> 1) + 1;
                //FLAC_LOG_INFO("nrOfChannels %i", nrOfChannels);
                FLACMetadataBlock->numChannels = nrOfChannels;

                bitsPerSample  =  (*(inbuf + pos + 12) & 0x01) << 5;
                bitsPerSample += ((*(inbuf + pos + 13) & 0xF0) >> 4) + 1;
                FLACMetadataBlock->bitsPerSample = bitsPerSample;
                //FLAC_LOG_INFO("bitsPerSample %i", bitsPerSample);

                totalSamplesInStream  = (uint64_t)(*(inbuf + pos + 13) & 0x0F) << 32;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 14)) << 24;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 15)) << 16;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 16)) << 8;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 17));
                //FLAC_LOG_INFO("totalSamplesInStream %lli", totalSamplesInStream);
                FLACMetadataBlock->totalSamples = totalSamplesInStream;

                //FLAC_LOG_INFO("nBytes %i, blockLength %i", nBytes, blockLength);
                pos += blockLength;
                nBytes -= blockLength;
                if(ret == FLAC_PARSE_OGG_DONE) return ret;
                break;

            case vorbisComment:                                // https://www.xiph.org/vorbis/doc/v-comment.html
                vendorLength  = *(inbuf + pos + 3) << 24;
                vendorLength += *(inbuf + pos + 2) << 16;
                vendorLength += *(inbuf + pos + 1) <<  8;
                vendorLength += *(inbuf + pos + 0);
                if(vendorLength > 1024){
                    FLAC_LOG_INFO("vendorLength > 1024 bytes");
                }
                m_flacVendorString.alloc(vendorLength + 1); m_flacVendorString.clear();
                m_flacVendorString.copy_from((char*)inbuf + pos + 4, vendorLength);
                // FLAC_LOG_VERBOSE("Vendor: %s", m_flacVendorString.c_get());

                pos += 4 + vendorLength;
                userCommentListLength  = *(inbuf + pos + 3) << 24;
                userCommentListLength += *(inbuf + pos + 2) << 16;
                userCommentListLength += *(inbuf + pos + 1) <<  8;
                userCommentListLength += *(inbuf + pos + 0);

                pos += 4;
                commemtStringLength = 0;
                for(int32_t i = 0; i < userCommentListLength; i++){
                    commemtStringLength  = *(inbuf + pos + 3) << 24;
                    commemtStringLength += *(inbuf + pos + 2) << 16;
                    commemtStringLength += *(inbuf + pos + 1) <<  8;
                    commemtStringLength += *(inbuf + pos + 0);

                    if((specialIndexOf(inbuf + pos + 4, "TITLE", 6) == 0) || (specialIndexOf(inbuf + pos + 4, "title", 6) == 0)){
                        vb[0].assign((const char*)(inbuf + pos + 4 + 6), min((uint32_t)127, commemtStringLength - 6));
                        // FLAC_LOG_VERBOSE("TITLE: %s", vb[0].c_get());
                    }
                    if((specialIndexOf(inbuf + pos + 4, "ARTIST", 7) == 0) || (specialIndexOf(inbuf + pos + 4, "artist", 7) == 0)){
                        vb[1].assign((const char*)(inbuf + pos + 4 + 7), min((uint32_t)127, commemtStringLength - 7));
                        // FLAC_LOG_VERBOSE("ARTIST: %s", vb[1].c_get());
                    }
                    if((specialIndexOf(inbuf + pos + 4, "GENRE", 6) == 0) || (specialIndexOf(inbuf + pos + 4, "genre", 6) == 0)){
                        vb[2].assign((const char*)(inbuf + pos + 4 + 6), min((uint32_t)127, commemtStringLength - 6));
                        FLAC_LOG_VERBOSE("GENRE: %s", vb[2].c_get());
                    }
                    if((specialIndexOf(inbuf + pos + 4, "ALBUM", 6) == 0) || (specialIndexOf(inbuf + pos + 4, "album", 6) == 0)){
                        vb[3].assign((const char*)(inbuf + pos + 4 + 6), min((uint32_t)127, commemtStringLength - 6));
                        FLAC_LOG_VERBOSE("ALBUM: %s", vb[3].c_get());
                    }
                    if((specialIndexOf(inbuf + pos + 4, "COMMENT", 8) == 0) || (specialIndexOf(inbuf + pos + 4, "comment", 8) == 0)){
                        vb[4].assign((const char*)(inbuf + pos + 4 + 8), min((uint32_t)127, commemtStringLength - 8));
                        FLAC_LOG_VERBOSE("COMMENT: %s", vb[4].c_get());
                    }
                    if((specialIndexOf(inbuf + pos + 4, "DATE", 5) == 0) || (specialIndexOf(inbuf + pos + 4, "date", 5) == 0)){
                        vb[5].assign((const char*)(inbuf + pos + 4 + 5), min((uint32_t)127, commemtStringLength - 12));
                        FLAC_LOG_VERBOSE("DATE: %s", vb[5].c_get());
                    }
                    if((specialIndexOf(inbuf + pos + 4, "TRACKNUMBER", 12) == 0) || (specialIndexOf(inbuf + pos + 4, "tracknumber", 12) == 0)){
                        vb[6].assign((const char*)(inbuf + pos + 4 + 12), min((uint32_t)127, commemtStringLength - 12));
                        FLAC_LOG_VERBOSE("TRACKNUMBER: %s", vb[6].c_get());
                    }
                    if((specialIndexOf(inbuf + pos + 4, "METADATA_BLOCK_PICTURE", 23) == 0) || (specialIndexOf(inbuf + pos + 4, "metadata_block_picture", 23) == 0)){
                        FLAC_LOG_VERBOSE("METADATA_BLOCK_PICTURE found, commemtStringLength %i", commemtStringLength);
                        m_flacBlockPicLen = commemtStringLength - 23;
                        m_flacBlockPicPos = m_flacCurrentFilePos + pos + 4 + 23;
                        m_flacBlockPicLenUntilFrameEnd = nBytes - (pos + 23);
                        if(m_flacBlockPicLen < m_flacBlockPicLenUntilFrameEnd) m_flacBlockPicLenUntilFrameEnd = m_flacBlockPicLen;
                        m_flacRemainBlockPicLen = m_flacBlockPicLen - m_flacBlockPicLenUntilFrameEnd;
                        //FLAC_LOG_INFO("s_flacBlockPicPos %i, m_flacBlockPicLen %i", m_flacBlockPicPos, m_flacBlockPicLen);
                        //FLAC_LOG_INFO("s_flacBlockPicLenUntilFrameEnd %i, m_flacRemainBlockPicLen %i", m_flacBlockPicLenUntilFrameEnd, m_flacRemainBlockPicLen);
                        if(m_flacRemainBlockPicLen <= 0) m_f_lastMetaDataBlock = true; // exeption:: goto audiopage after commemt if lastMetaDataFlag is not set
                        if(m_flacBlockPicLen){
                            m_flacBlockPicItem.clear();
                            m_flacBlockPicItem.shrink_to_fit();
                            m_flacBlockPicItem.push_back(m_flacBlockPicPos);
                            m_flacBlockPicItem.push_back(m_flacBlockPicLenUntilFrameEnd);
                        }
                    }
                    pos += 4 + commemtStringLength;
                    // FLAC_LOG_VERBOSE("nBytes %i, pos %i, commemtStringLength %i", nBytes, pos, commemtStringLength);
                }
                if(vb[1].valid() && vb[0].valid()){ // artist and title
                    m_flacStreamTitle.assign(vb[1].c_get());
                    m_flacStreamTitle.append(" - ");
                    m_flacStreamTitle.append(vb[0].c_get());
                    m_f_flacNewStreamtitle = true;
                }
                else if(vb[1].valid()){
                    m_flacStreamTitle.assign(vb[1].c_get());
                    m_f_flacNewStreamtitle = true;
                }
                else if(vb[0].valid()){
                    m_flacStreamTitle.assign(vb[0].c_get());
                    m_f_flacNewStreamtitle = true;
                }
                if(!m_flacBlockPicLen && m_flacSegmTableVec.size() == 1) m_f_lastMetaDataBlock = true; // exeption:: goto audiopage after commemt if lastMetaDataFlag is not set
                if(ret == FLAC_PARSE_OGG_DONE) return ret;
                break;

            case picture:
                if(ret == FLAC_PARSE_OGG_DONE) return ret;
                break;

            default:
                return ret;
                break;
        }
    }
    return 0;
}
//----------------------------------------------------------------------------------------------------------------------
int32_t FlacDecoder::decode(uint8_t *inbuf, int32_t *bytesLeft, int16_t *outbuf){ //  MAIN LOOP

    int32_t                ret = 0;
    uint32_t           segmLen = 0;
    static uint32_t segmLenTmp = 0;

    if(m_f_flacFirstCall){ // determine if ogg or flag
        m_f_flacFirstCall = false;
        m_nBytes = 0;
        segmLenTmp = 0;
        if(specialIndexOf(inbuf, "OggS", 5) == 0){
            m_f_oggWrapper = true;
            m_f_flacParseOgg = true;
        }
    }

    if(m_f_oggWrapper){

        if(segmLenTmp){ // can't skip more than 16K
            if(segmLenTmp > FLAC_MAX_BLOCKSIZE){
                m_flacCurrentFilePos += FLAC_MAX_BLOCKSIZE;
                *bytesLeft -= FLAC_MAX_BLOCKSIZE;
                segmLenTmp -= FLAC_MAX_BLOCKSIZE;
            }
            else{
                m_flacCurrentFilePos += segmLenTmp;
                *bytesLeft -= segmLenTmp;
                segmLenTmp  = 0;
            }
            return FLAC_PARSE_OGG_DONE;
        }

        if(m_nBytes > 0){
            int16_t diff = m_nBytes;
            if(m_flacAudioDataStart == 0){
                m_flacAudioDataStart = m_flacCurrentFilePos;
            }
            ret = decodeNative(inbuf, &m_nBytes, outbuf);
            diff -= m_nBytes;
            m_flacCurrentFilePos += diff;
            *bytesLeft -= diff;
            return ret;
        }
        if(m_nBytes < 0){FLAC_LOG_ERROR("Flac decoder asynchron"); return FLAC_ERR;}

        if(m_f_flacParseOgg == true){
            m_f_flacParseOgg = false;
            ret = parseOGG(inbuf, bytesLeft);
            if(ret == FLAC_NONE) return FLAC_PARSE_OGG_DONE; // ok
            else return ret;  // error
        }
        //-------------------------------------------------------
        if(!m_flacSegmTableVec.size()) FLAC_LOG_ERROR("size is 0");
        segmLen = m_flacSegmTableVec.back();
        m_flacSegmTableVec.pop_back();
        if(!m_flacSegmTableVec.size()) m_f_flacParseOgg = true;
        //-------------------------------------------------------

        if(m_flacRemainBlockPicLen <= 0 && !m_f_flacNewMetadataBlockPicture) {
            if(m_flacBlockPicItem.size() > 0) { // get blockpic data
                // FLAC_LOG_INFO("---------------------------------------------------------------------------");
                // FLAC_LOG_INFO("metadata blockpic found at pos %i, size %i bytes", m_flacBlockPicPos, m_flacBlockPicLen);
                // for(int32_t i = 0; i < m_flacBlockPicItem.size(); i += 2) { FLAC_LOG_INFO("segment %02i, pos %07i, len %05i", i / 2, m_flacBlockPicItem[i], m_flacBlockPicItem[i + 1]); }
                // FLAC_LOG_INFO("---------------------------------------------------------------------------");
                m_f_flacNewMetadataBlockPicture = true;
            }
        }

        switch(m_flacPageNr) {
            case 0:
                ret = parseFlacFirstPacket(inbuf, segmLen);
                if(ret == segmLen) {
                    m_flacPageNr = 1;
                    ret = FLAC_PARSE_OGG_DONE;
                    break;
                }
                if(ret < 0){  // fLaC signature not found
                    break;
                }
                if(ret < segmLen){
                    segmLen -= ret;
                    *bytesLeft -= ret;
                    m_flacCurrentFilePos += ret;
                    inbuf += ret;
                    m_flacPageNr = 1;
                } /* fallthrough */
            case 1:
                if(m_flacRemainBlockPicLen > 0){
                    m_flacRemainBlockPicLen -= segmLen;
                    //FLAC_LOG_INFO("s_flacCurrentFilePos %i, len %i, m_flacRemainBlockPicLen %i", m_flacCurrentFilePos, segmLen, m_flacRemainBlockPicLen);
                    m_flacBlockPicItem.push_back(m_flacCurrentFilePos);
                    m_flacBlockPicItem.push_back(segmLen);
                    if(m_flacRemainBlockPicLen <= 0){m_flacPageNr = 2;}
                    ret = FLAC_PARSE_OGG_DONE;
                    break;
                }
                ret = parseMetaDataBlockHeader(inbuf, segmLen);
                if(m_f_lastMetaDataBlock) m_flacPageNr = 2;
                break;
            case 2:
                m_nBytes = segmLen;
                return FLAC_PARSE_OGG_DONE;
                break;
        }
        if(segmLen > FLAC_MAX_BLOCKSIZE){
            segmLenTmp = segmLen;
            return FLAC_PARSE_OGG_DONE;
        }
        *bytesLeft -= segmLen;
        m_flacCurrentFilePos += segmLen;
        return ret;
    }
    ret = decodeNative(inbuf, bytesLeft, outbuf);
    return ret;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FlacDecoder::decodeNative(uint8_t *inbuf, int32_t *bytesLeft, int16_t *outbuf){

    int32_t bl = *bytesLeft;
    static int32_t sbl = 0;

    if(m_flacStatus != OUT_SAMPLES){
        m_rIndex = 0;
        m_flacInptr = inbuf;
    }

    while(m_flacStatus == DECODE_FRAME){// Read a ton of header fields, and ignore most of them
        int32_t ret = decodeFrame (inbuf, bytesLeft);
        if(ret != 0) return ret;
        if(*bytesLeft < FLAC_MAX_BLOCKSIZE) return FLAC_DECODE_FRAMES_LOOP; // need more data
        sbl += bl - *bytesLeft;
    }

    if(m_flacStatus == DECODE_SUBFRAMES){
        // Decode each channel's subframe, then skip footer
        int32_t ret = decodeSubframes(bytesLeft);
        if(ret != 0) return ret;
        m_flacStatus = OUT_SAMPLES;
        sbl += bl - *bytesLeft;
    }

    if(m_flacStatus == OUT_SAMPLES){  // Write the decoded samples
        // blocksize can be much greater than outbuff, so we can't stuff all in once
        // therefore we need often more than one loop (split outputblock into pieces)
        uint32_t blockSize;
        if(m_numOfOutSamples < m_flacOutBuffSize + m_offset) blockSize = m_numOfOutSamples - m_offset;
        else blockSize = m_flacOutBuffSize;

        for (int32_t i = 0; i < blockSize; i++) {
            for (int32_t j = 0; j < FLACMetadataBlock->numChannels; j++) {
                int32_t val = m_samplesBuffer[j][i + m_offset];
                if (FLACMetadataBlock->bitsPerSample == 8) val += 128;
                outbuf[2*i+j] = val;
            }
        }

        m_flacValidSamples = blockSize * FLACMetadataBlock->numChannels;
        m_offset += blockSize;
        if(sbl > 0){
            m_flacCompressionRatio = (float)((m_flacValidSamples * 2) * FLACMetadataBlock->numChannels) / sbl; // valid samples are 16 bit
            sbl = 0;
            m_flacBitrate = FLACMetadataBlock->sampleRate * FLACMetadataBlock->bitsPerSample * FLACMetadataBlock->numChannels;
            m_flacBitrate /= m_flacCompressionRatio;
      //      FLAC_LOG_INFO("s_flacBitrate %i, m_flacCompressionRatio %f, FLACMetadataBlock->sampleRate %i ", m_flacBitrate, m_flacCompressionRatio, FLACMetadataBlock->sampleRate);
        }
        if(m_offset != m_numOfOutSamples) return GIVE_NEXT_LOOP;
        if(m_offset > m_numOfOutSamples) { FLAC_LOG_ERROR("offset has a wrong value"); }
        m_offset = 0;
    }

    alignToByte();
    readUint(16, bytesLeft);

//    m_flacCompressionRatio = (float)m_bytesDecoded / (float)s_numOfOutSamples * FLACMetadataBlock->numChannels * (16/8);
//    FLAC_LOG_INFO("s_flacCompressionRatio % f", m_flacCompressionRatio);
    m_flacStatus = DECODE_FRAME;
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FlacDecoder::decodeFrame(uint8_t *inbuf, int32_t *bytesLeft){
    if(specialIndexOf(inbuf, "OggS", *bytesLeft) == 0){ // async? => new sync is OggS => reset and decode (not page 0 or 1)
        decoderReset();
        m_flacPageNr = 2;
        return FLAC_OGG_SYNC_FOUND;
    }
    readUint(14 + 1, bytesLeft); // synccode + reserved bit
    FLACFrameHeader->blockingStrategy = readUint(1, bytesLeft);
    FLACFrameHeader->blockSizeCode = readUint(4, bytesLeft);
    FLACFrameHeader->sampleRateCode = readUint(4, bytesLeft);
    FLACFrameHeader->chanAsgn = readUint(4, bytesLeft);
    FLACFrameHeader->sampleSizeCode = readUint(3, bytesLeft);
    if(!FLACMetadataBlock->numChannels){
        if(FLACFrameHeader->chanAsgn == 0) FLACMetadataBlock->numChannels = 1;
        if(FLACFrameHeader->chanAsgn == 1) FLACMetadataBlock->numChannels = 2;
        if(FLACFrameHeader->chanAsgn > 7)  FLACMetadataBlock->numChannels = 2;
    }
    if(FLACMetadataBlock->numChannels < 1) {FLAC_LOG_ERROR("Flac unknown channel assignment, ch: %i", FLACMetadataBlock->numChannels); return FLAC_STOP;}
        if(!FLACMetadataBlock->bitsPerSample){
        if(FLACFrameHeader->sampleSizeCode == 1) FLACMetadataBlock->bitsPerSample =  8;
        if(FLACFrameHeader->sampleSizeCode == 2) FLACMetadataBlock->bitsPerSample = 12;
        if(FLACFrameHeader->sampleSizeCode == 4) FLACMetadataBlock->bitsPerSample = 16;
        if(FLACFrameHeader->sampleSizeCode == 5) FLACMetadataBlock->bitsPerSample = 20;
        if(FLACFrameHeader->sampleSizeCode == 6) FLACMetadataBlock->bitsPerSample = 24;
    }
    if(FLACMetadataBlock->bitsPerSample > 16) {FLAC_LOG_ERROR("Flac, bits per sample > 16, bps: %i", FLACMetadataBlock->bitsPerSample); return FLAC_STOP;}
    if(FLACMetadataBlock->bitsPerSample < 8 ) {FLAC_LOG_ERROR("Flac, bits per sample <8, bps: %i", FLACMetadataBlock->bitsPerSample); return FLAC_STOP;}
    if(!FLACMetadataBlock->sampleRate){
        if(FLACFrameHeader->sampleRateCode == 1)  FLACMetadataBlock->sampleRate =  88200;
        if(FLACFrameHeader->sampleRateCode == 2)  FLACMetadataBlock->sampleRate = 176400;
        if(FLACFrameHeader->sampleRateCode == 3)  FLACMetadataBlock->sampleRate = 192000;
        if(FLACFrameHeader->sampleRateCode == 4)  FLACMetadataBlock->sampleRate =   8000;
        if(FLACFrameHeader->sampleRateCode == 5)  FLACMetadataBlock->sampleRate =  16000;
        if(FLACFrameHeader->sampleRateCode == 6)  FLACMetadataBlock->sampleRate =  22050;
        if(FLACFrameHeader->sampleRateCode == 7)  FLACMetadataBlock->sampleRate =  24000;
        if(FLACFrameHeader->sampleRateCode == 8)  FLACMetadataBlock->sampleRate =  32000;
        if(FLACFrameHeader->sampleRateCode == 9)  FLACMetadataBlock->sampleRate =  44100;
        if(FLACFrameHeader->sampleRateCode == 10) FLACMetadataBlock->sampleRate =  48000;
        if(FLACFrameHeader->sampleRateCode == 11) FLACMetadataBlock->sampleRate =  96000;
    }
    readUint(1, bytesLeft);
    uint32_t temp = (readUint(8, bytesLeft) << 24);
    temp = ~temp;
    uint32_t shift = 0x80000000; // Number of leading zeros
    int8_t count = 0;
    for(int32_t i=0; i<32; i++){
        if((temp & shift) == 0) {count++; shift >>= 1;}
        else break;
    }
    count--;
    for (int32_t i = 0; i < count; i++) readUint(8, bytesLeft);
    m_numOfOutSamples = 0;
    if (FLACFrameHeader->blockSizeCode == 1)
        m_numOfOutSamples = 192;
    else if (2 <= FLACFrameHeader->blockSizeCode && FLACFrameHeader->blockSizeCode <= 5)
        m_numOfOutSamples = 576 << (FLACFrameHeader->blockSizeCode - 2);
    else if (FLACFrameHeader->blockSizeCode == 6)
        m_numOfOutSamples = readUint(8, bytesLeft) + 1;
    else if (FLACFrameHeader->blockSizeCode == 7)
        m_numOfOutSamples = readUint(16, bytesLeft) + 1;
    else if (8 <= FLACFrameHeader->blockSizeCode && FLACFrameHeader->blockSizeCode <= 15)
        m_numOfOutSamples = 256 << (FLACFrameHeader->blockSizeCode - 8);
    else{
        FLAC_LOG_ERROR("Flac, reserved blocksize unsupported, block size code: %i", FLACFrameHeader->blockSizeCode);
        return FLAC_ERR;
    }
    if(m_numOfOutSamples > FLAC_MAX_OUTBUFFSIZE){
        FLAC_LOG_ERROR("Flac, blockSizeOut too big ,%i bytes", m_numOfOutSamples);
        return FLAC_ERR;
    }
    if(FLACFrameHeader->sampleRateCode == 12)
        readUint(8, bytesLeft);
    else if (FLACFrameHeader->sampleRateCode == 13 || FLACFrameHeader->sampleRateCode == 14){
        readUint(16, bytesLeft);
    }
    readUint(8, bytesLeft);
    m_flacStatus = DECODE_SUBFRAMES;
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FlacDecoder::getOutputSamples(){
    uint32_t vs = m_flacValidSamples;
    m_flacValidSamples=0;
    return vs;
}
//----------------------------------------------------------------------------------------------------------------------
uint64_t FlacDecoder::getTotoalSamplesInStream(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->totalSamples;
}
//----------------------------------------------------------------------------------------------------------------------
uint8_t FlacDecoder::getBitsPerSample(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->bitsPerSample;
}
//----------------------------------------------------------------------------------------------------------------------
uint8_t FlacDecoder::getChannels(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->numChannels;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FlacDecoder::getSampleRate(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->sampleRate;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FlacDecoder::getBitRate(){
    return m_flacBitrate;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FlacDecoder::getAudioDataStart(){
    return m_flacAudioDataStart;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FlacDecoder::getAudioFileDuration() {
    if(getSampleRate()){ // DIV0
        uint32_t afd = getTotoalSamplesInStream()/ getSampleRate(); // AudioFileDuration
        return afd;
    }
    return 0;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FlacDecoder::decodeSubframes(int32_t* bytesLeft){
    if(FLACFrameHeader->chanAsgn <= 7) {
        for (int32_t ch = 0; ch < FLACMetadataBlock->numChannels; ch++)
            decodeSubframe(FLACMetadataBlock->bitsPerSample, ch, bytesLeft);
    }
    else if (8 <= FLACFrameHeader->chanAsgn && FLACFrameHeader->chanAsgn <= 10) {
        decodeSubframe(FLACMetadataBlock->bitsPerSample + (FLACFrameHeader->chanAsgn == 9 ? 1 : 0), 0, bytesLeft);
        decodeSubframe(FLACMetadataBlock->bitsPerSample + (FLACFrameHeader->chanAsgn == 9 ? 0 : 1), 1, bytesLeft);
        if(FLACFrameHeader->chanAsgn == 8) {
            for (int32_t i = 0; i < m_numOfOutSamples; i++)
                m_samplesBuffer[1][i] = (
                        m_samplesBuffer[0][i] -
                        m_samplesBuffer[1][i]);
        }
        else if (FLACFrameHeader->chanAsgn == 9) {
            for (int32_t i = 0; i < m_numOfOutSamples; i++)
                m_samplesBuffer[0][i] += m_samplesBuffer[1][i];
        }
        else if (FLACFrameHeader->chanAsgn == 10) {
            for (int32_t i = 0; i < m_numOfOutSamples; i++) {
                int32_t side =  m_samplesBuffer[1][i];
                int32_t right = m_samplesBuffer[0][i] - (side >> 1);
                m_samplesBuffer[1][i] = right;
                m_samplesBuffer[0][i] = right + side;
            }
        }
        else {
            FLAC_LOG_ERROR("Flac, unknown channel assignment, %i", FLACFrameHeader->chanAsgn);
            return FLAC_ERR;
        }
    }
    else{
        FLAC_LOG_ERROR("Flac reserved channel assignment, %i", FLACFrameHeader->chanAsgn);
        return FLAC_ERR;
    }
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FlacDecoder::decodeSubframe(uint8_t sampleDepth, uint8_t ch, int32_t* bytesLeft) {
    int8_t ret = 0;
    readUint(1, bytesLeft);                // Zero bit padding, to prevent sync-fooling string of 1s
    uint8_t type = readUint(6, bytesLeft); // Subframe type: 000000 : SUBFRAME_CONSTANT
                                           //                000001 : SUBFRAME_VERBATIM
                                           //                00001x : reserved
                                           //                0001xx : reserved
                                           //                001xxx : if(xxx <= 4) SUBFRAME_FIXED, xxx=order ; else reserved
                                           //                01xxxx : reserved
                                           //                1xxxxx : SUBFRAME_LPC, xxxxx=order-1

    int32_t shift = readUint(1, bytesLeft);    // Wasted bits-per-sample' flag:
                                           // 0 : no wasted bits-per-sample in source subblock, k=0
                                           // 1 : k wasted bits-per-sample in source subblock, k-1 follows, unary coded; e.g. k=3 => 001 follows, k=7 => 0000001 follows.
    if (shift == 1) {
        while (readUint(1, bytesLeft) == 0) { shift++;}
    }
    sampleDepth -= shift;

    if(type == 0){  // Constant coding
        int32_t s= readSignedInt(sampleDepth, bytesLeft);                                    // SUBFRAME_CONSTANT
        for(int32_t i = 0; i < m_numOfOutSamples; i++){
            m_samplesBuffer[ch][i] = s;
        }
    }
    else if (type == 1) {  // Verbatim coding
        for (int32_t i = 0; i < m_numOfOutSamples; i++)
            m_samplesBuffer[ch][i] = readSignedInt(sampleDepth, bytesLeft);                  // SUBFRAME_VERBATIM
    }
    else if (8 <= type && type <= 12){
        ret = decodeFixedPredictionSubframe(type - 8, sampleDepth, ch, bytesLeft);           // SUBFRAME_FIXED
        if(ret) return ret;
    }
    else if (32 <= type && type <= 63){
        ret = decodeLinearPredictiveCodingSubframe(type - 31, sampleDepth, ch, bytesLeft);   // SUBFRAME_LPC
        if(ret) return ret;
    }
    else{
        FLAC_LOG_ERROR("Flac unimplemented reserved subtype: %i", type);
        return FLAC_ERR;
    }
    if(shift>0){
        for (int32_t i = 0; i < m_numOfOutSamples; i++){
            m_samplesBuffer[ch][i] <<= shift;
        }
    }
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
int8_t FlacDecoder::decodeFixedPredictionSubframe(uint8_t predOrder, uint8_t sampleDepth, uint8_t ch, int32_t* bytesLeft) {     // SUBFRAME_FIXED

    uint8_t ret = 0;
    for(uint8_t i = 0; i < predOrder; i++)
        m_samplesBuffer[ch][i] = readSignedInt(sampleDepth, bytesLeft); // Unencoded warm-up samples (n = frame's bits-per-sample * predictor order).
    ret = decodeResiduals(predOrder, ch, bytesLeft);
    if(ret) return ret;
    coefs.clear(); coefs.shrink_to_fit();
    if(predOrder == 0) coefs.resize(0);
    if(predOrder == 1) coefs.push_back(1);  // FIXED_PREDICTION_COEFFICIENTS
    if(predOrder == 2){coefs.push_back(2); coefs.push_back(-1);}
    if(predOrder == 3){coefs.push_back(3); coefs.push_back(-3); coefs.push_back(1);}
    if(predOrder == 4){coefs.push_back(4); coefs.push_back(-6); coefs.push_back(4); coefs.push_back(-1);}
    if(predOrder > 4) {FLAC_LOG_ERROR("Flac preorder too big: %i", predOrder); return FLAC_ERR;} // Error: preorder > 4"
    restoreLinearPrediction(ch, 0);
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FlacDecoder::decodeLinearPredictiveCodingSubframe(int32_t lpcOrder, int32_t sampleDepth, uint8_t ch, int32_t* bytesLeft){

    int8_t ret = 0;
    for (int32_t i = 0; i < lpcOrder; i++){
        m_samplesBuffer[ch][i] = readSignedInt(sampleDepth, bytesLeft); // Unencoded warm-up samples (n = frame's bits-per-sample * lpc order).
    }
    int32_t precision = readUint(4, bytesLeft) + 1;                         // (Quantized linear predictor coefficients' precision in bits)-1 (1111 = invalid).
    int32_t shift = readSignedInt(5, bytesLeft);                            // Quantized linear predictor coefficient shift needed in bits (NOTE: this number is signed two's-complement).
    coefs.clear(); coefs.shrink_to_fit();
    for (uint8_t i = 0; i < lpcOrder; i++){
        coefs.push_back(readSignedInt(precision, bytesLeft));           // Unencoded predictor coefficients (n = qlp coeff precision * lpc order) (NOTE: the coefficients are signed two's-complement).
    }
    ret = decodeResiduals(lpcOrder, ch, bytesLeft);
    if(ret) return ret;
    restoreLinearPrediction(ch, shift);
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FlacDecoder::decodeResiduals(uint8_t warmup, uint8_t ch, int32_t* bytesLeft) {

    int32_t method = readUint(2, bytesLeft);                          // Residual coding method:
                                                                  // 00 : partitioned Rice coding with 4-bit Rice parameter; RESIDUAL_CODING_METHOD_PARTITIONED_RICE follows
                                                                  // 01 : partitioned Rice coding with 5-bit Rice parameter; RESIDUAL_CODING_METHOD_PARTITIONED_RICE2 follows
                                                                  // 10-11 : reserved
    if (method >= 2) {FLAC_LOG_ERROR("Flac reserved residual coding, method: %i", method); return FLAC_ERR;}
    uint8_t paramBits = method == 0 ? 4 : 5;                      // RESIDUAL_CODING_METHOD_PARTITIONED_RICE || RESIDUAL_CODING_METHOD_PARTITIONED_RICE2
    int32_t escapeParam = ( method == 0 ? 0xF : 0x1F);
    int32_t partitionOrder = readUint(4, bytesLeft);                  // Partition order
    int32_t numPartitions = 1 << partitionOrder;                      // There will be 2^order partitions.

    if (m_numOfOutSamples % numPartitions != 0){
        FLAC_LOG_ERROR("Flac, wrong rice partition number");
        return FLAC_ERR;                  //Error: Block size not divisible by number of Rice partitions
    }
    int32_t partitionSize = m_numOfOutSamples / numPartitions;

    for (int32_t i = 0; i < numPartitions; i++) {
        int32_t start = i * partitionSize + (i == 0 ? warmup : 0);
        int32_t end = (i + 1) * partitionSize;

        int32_t param = readUint(paramBits, bytesLeft);
        if (param < escapeParam) {
            for (int32_t j = start; j < end; j++){
                if(m_f_bitReaderError) break;
                m_samplesBuffer[ch][j] = readRiceSignedInt(param, bytesLeft);
            }
        }
        else {
            int32_t numBits = readUint(5, bytesLeft);                 // Escape code, meaning the partition is in unencoded binary form using n bits per sample; n follows as a 5-bit number.
            for (int32_t j = start; j < end; j++){
                if(m_f_bitReaderError) break;
                m_samplesBuffer[ch][j] = readSignedInt(numBits, bytesLeft);
            }
        }
    }
    if(m_f_bitReaderError) {FLAC_LOG_ERROR("Flac bitreader underflow"); return FLAC_ERR;}
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
void FlacDecoder::restoreLinearPrediction(uint8_t ch, uint8_t shift) {

    for (int32_t i = coefs.size(); i < m_numOfOutSamples; i++) {
        int32_t sum = 0;
        for (int32_t j = 0; j < coefs.size(); j++){
            sum += m_samplesBuffer[ch][i - 1 - j] * coefs[j];
        }
        m_samplesBuffer[ch][i] += (sum >> shift);
    }
}
//----------------------------------------------------------------------------------------------------------------------
int32_t FlacDecoder::specialIndexOf(uint8_t* base, const char* str, int32_t baselen, bool exact){
    int32_t result = 0;  // seek for str in buffer or in header up to baselen, not nullterninated
    if (strlen(str) > baselen) return -1; // if exact == true seekstr in buffer must have "\0" at the end
    for (int32_t i = 0; i < baselen - strlen(str); i++){
        result = i;
        for (int32_t j = 0; j < strlen(str) + exact; j++){
            if (*(base + i + j) != *(str + j)){
                result = -1;
                break;
            }
        }
        if (result >= 0) break;
    }
    return result;
}
//----------------------------------------------------------------------------------------------------------------------
const char* FlacDecoder::arg1() {return nullptr;} // virtual method
const char* FlacDecoder::arg2() {return nullptr;} // virtual method
