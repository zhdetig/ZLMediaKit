/*
 * RtmpParser.cpp
 *
 *  Created on: 2016年12月2日
 *      Author: xzl
 */

#include "RtmpParser.h"

namespace ZL {
namespace Rtmp {

RtmpParser::RtmpParser(const AMFValue &val) {
	auto videoCodec = val["videocodecid"];
	auto audioCodec = val["audiocodecid"];
    
	if (videoCodec.type() == AMF_STRING) {
        if (videoCodec.as_string() == "avc1") {
            //264
            m_bHaveVideo = true;
        } else {
            InfoL << "不支持RTMP视频格式:" << videoCodec.as_string();
        }
    }else if (videoCodec.type() != AMF_NULL){
        if (videoCodec.as_integer() == 7) {
            //264
            m_bHaveVideo = true;
        } else {
            InfoL << "不支持RTMP视频格式:" << videoCodec.as_integer();
        }
    }
    
    
	if (audioCodec.type() == AMF_STRING) {
		if (audioCodec.as_string() == "mp4a") {
			//aac
			m_bHaveAudio = true;
		} else {
			InfoL << "不支持RTMP音频格式:" << audioCodec.as_string();
		}
    }else if (audioCodec.type() != AMF_NULL) {
        if (audioCodec.as_integer() == 10) {
            //aac
            m_bHaveAudio = true;
        } else {
            InfoL << "不支持RTMP音频格式:" << audioCodec.as_integer();
        }
    }
    
    
    if (!m_bHaveVideo && !m_bHaveAudio) {
        throw std::runtime_error("不支持该RTMP媒体格式");
    }
    
	onCheckMedia(val);
}

RtmpParser::~RtmpParser() {
	// TODO Auto-generated destructor stub
}

bool RtmpParser::inputRtmp(const RtmpPacket &pkt) {
	switch (pkt.typeId) {
	case MSG_VIDEO:
		if (m_bHaveVideo) {
			return inputVideo(pkt);
		}
		return false;
	case MSG_AUDIO:
		if (m_bHaveAudio) {
			return inputAudio(pkt);
		}
		return false;
	default:
		return false;
	}
}

inline bool RtmpParser::inputVideo(const RtmpPacket& pkt) {
	if (pkt.isCfgFrame()) {
		//WarnL << " got h264 cfg";
		if (m_strSPS.size()) {
			return false;
		}
		m_strSPS.assign("\x00\x00\x00\x01", 4);
		m_strSPS.append(pkt.getH264SPS());

		m_strPPS.assign("\x00\x00\x00\x01", 4);
		m_strPPS.append(pkt.getH264PPS());

		getAVCInfo(pkt.getH264SPS(), m_iVideoWidth, m_iVideoHeight, m_fVideoFps);
		return false;
	}

	if (m_strSPS.size()) {
		uint32_t iTotalLen = pkt.strBuf.size();
		uint32_t iOffset = 5;
		while(iOffset + 4 < iTotalLen){
            uint32_t iFrameLen;
            memcpy(&iFrameLen, pkt.strBuf.data() + iOffset, 4);
            iFrameLen = ntohl(iFrameLen);
			iOffset += 4;
			if(iFrameLen + iOffset > iTotalLen){
				break;
			}
			_onGetH264(pkt.strBuf.data() + iOffset, iFrameLen, pkt.timeStamp);
			iOffset += iFrameLen;
		}
	}
	return  pkt.isVideoKeyFrame();
}
inline void RtmpParser::_onGetH264(const char* pcData, int iLen, uint32_t ui32TimeStamp) {
	switch (pcData[0] & 0x1F) {
	case 5: {
		onGetH264(m_strSPS.data() + 4, m_strSPS.length() - 4, ui32TimeStamp);
		onGetH264(m_strPPS.data() + 4, m_strPPS.length() - 4, ui32TimeStamp);
	}
	case 1: {
		onGetH264(pcData, iLen, ui32TimeStamp);
	}
		break;
	default:
		//WarnL <<(int)(pcData[0] & 0x1F);
		break;
	}
}
inline void RtmpParser::onGetH264(const char* pcData, int iLen, uint32_t ui32TimeStamp) {
	m_h264frame.type = pcData[0] & 0x1F;
	m_h264frame.timeStamp = ui32TimeStamp;
	m_h264frame.data.assign("\x0\x0\x0\x1", 4);  //添加264头
	m_h264frame.data.append(pcData, iLen);
	{
		lock_guard<recursive_mutex> lck(m_mtxCB);
		if (onVideo) {
			onVideo(m_h264frame);
		}
	}
	m_h264frame.data.clear();
}

inline bool RtmpParser::inputAudio(const RtmpPacket& pkt) {
	if (pkt.isCfgFrame()) {
		if (m_strAudioCfg.size()) {
			return false;
		}
		m_strAudioCfg = pkt.getAacCfg();
		m_iSampleBit = pkt.getAudioSampleBit();
		makeAdtsHeader(m_strAudioCfg,m_adts);
		getAACInfo(m_adts, m_iSampleRate, m_iChannel);
		return false;
	}
	if (m_strAudioCfg.size()) {
		onGetAAC(pkt.strBuf.data() + 2, pkt.strBuf.size() - 2, pkt.timeStamp);
	}
	return false;
}
inline void RtmpParser::onGetAAC(const char* pcData, int iLen, uint32_t ui32TimeStamp) {
	//添加adts头
	memcpy(m_adts.data + 7, pcData, iLen);
	m_adts.aac_frame_length = 7 + iLen;
    m_adts.timeStamp = ui32TimeStamp;
    writeAdtsHeader(m_adts, m_adts.data);
	{
		lock_guard<recursive_mutex> lck(m_mtxCB);
		if (onAudio) {
			onAudio(m_adts);
		}
	}
	m_adts.aac_frame_length = 7;

}
inline void RtmpParser::onCheckMedia(const AMFValue& obj) {
	obj.object_for_each([&](const string &key ,const AMFValue& val) {
		if(key == "duration") {
			m_fDuration = val.as_number();
			return;
		}
		if(key == "width") {
			m_iVideoWidth = val.as_number();
			return;
		}
		if(key == "height") {
			m_iVideoHeight = val.as_number();
			return;
		}
		if(key == "framerate") {
			m_fVideoFps = val.as_number();
			return;
		}
		if(key == "audiosamplerate") {
			m_iSampleRate = val.as_number();
			return;
		}
		if(key == "audiosamplesize") {
			m_iSampleBit = val.as_number();
			return;
		}
		if(key == "stereo") {
			m_iChannel = val.as_boolean() ? 2 :1;
			return;
		}
	});
}


























} /* namespace Rtmp */
} /* namespace ZL */
