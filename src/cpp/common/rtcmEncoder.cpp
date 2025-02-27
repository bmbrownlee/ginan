
// #pragma GCC optimize ("O0")

#include <boost/log/trivial.hpp>

#include "rtcmEncoder.hpp"
#include "acsConfig.hpp"
#include "constants.hpp"


void calculateSsrComb(
	GTime 						curTime,
	int 						period,
	SSRMeta 					ssrMeta,
	int 						masterIod,
	map<SatSys, SSROut>&		ssrOutMap)
{
	if (ssrOutMap.empty())	
	{
		BOOST_LOG_TRIVIAL(warning) << "Warning: No suitable Ephemeris data available.";
		return;
	}

	map<E_BiasGroup, std::array<double,2>> commonClockOffsetsMap;	// 0: bias; 1: bias rate
	map<E_BiasGroup, std::array<GTime, 2>> commonClockEpochsMap;	// 0: 1st straddle pt epoch; 1: 2nd straddle pt epoch

	for (auto& [Sat, ssrOut] : ssrOutMap)
	{
		if (ssrOut.clkInput.vals[0].iode != ssrOut.ephInput.vals[0].iode)
		{
			tracepdeex(3, std::cout, "IODE mismatch between clock and ephemeris for %s\n", Sat.id().c_str());
			continue;
		}
		
		auto& ssrEph		= ssrOut.ssrEph;
		auto& ssrClk		= ssrOut.ssrClk;
		auto& ssrEphInput	= ssrOut.ephInput;
		auto& ssrClkInput	= ssrOut.clkInput;
		
		ssrEph.t0 					= curTime + period / 2;
		ssrClk.t0 					= curTime + period / 2;
		ssrEph.udi					= period;
		ssrClk.udi					= period;
		ssrEph.iod					= masterIod;
		ssrClk.iod					= masterIod;
		
		ssrEph.ssrMeta				= ssrMeta;
		ssrClk.ssrMeta				= ssrMeta;

		ssrEph.iode		= ssrEphInput.vals[0].iode;
		
		// ssrEph.iodcrc				= ??
		
		double		clkCorrections[2];
		Vector3d	posCorrections[2];
		
		for (int i = 0; i < 2; i++)
		{
			posCorrections[i]	= ssrEphInput.vals[i].brdcPos
								- ssrEphInput.vals[i].precPos;
			clkCorrections[i]	= ssrClkInput.vals[i].brdcClk
								- ssrClkInput.vals[i].precClk;
		}
		
		if (acsConfig.ssrOpts.extrapolate_corrections)
		{
			Vector3d	diffRAC[2];
			double		diffClock[2];
		
			for (int dt : {0, 1})
			{
				double		ephRatio	= (ssrEph.t0 + dt - ssrEphInput.vals[0].time) / (ssrEphInput.vals[1].time - ssrEphInput.vals[0].time);
				double		clkRatio	= (ssrClk.t0 + dt - ssrClkInput.vals[0].time) / (ssrClkInput.vals[1].time - ssrClkInput.vals[0].time);
			
				Vector3d	posCorrection	= posCorrections[0] 			+ ephRatio	* (posCorrections[1]			- posCorrections[0]);
				double		clkCorrection	= clkCorrections[0]				+ clkRatio	* (clkCorrections[1]			- clkCorrections[0]);
			
				Vector3d	satPosition		= ssrEphInput.vals[0].brdcPos	+ ephRatio	* (ssrEphInput.vals[1].brdcPos	- ssrEphInput.vals[0].brdcPos);
				Vector3d	satVelocity		= ssrEphInput.vals[0].brdcVel	+ ephRatio	* (ssrEphInput.vals[1].brdcVel	- ssrEphInput.vals[0].brdcVel);
			
				Vector3d	diffRac			= ecef2rac(satPosition, satVelocity) * posCorrection;
			
				diffRAC		[dt]	= diffRac;
				diffClock	[dt]	= -clkCorrection;
			}
			
			ssrEph. deph	= diffRAC[0]; 
			ssrEph.ddeph	= diffRAC[1] - diffRAC[0];
	
			ssrClk.dclk[0]	= diffClock[0];
			ssrClk.dclk[1]	= 0; //diffClock[1] - diffClock[0];
			ssrClk.dclk[2]	= 0;	// set to zero (not used)
		}
		else
		{
			ssrEph.deph		= ecef2rac(ssrEphInput.vals[1].brdcPos, ssrEphInput.vals[1].brdcVel) * posCorrections[1];
			ssrClk.dclk[0]	= -clkCorrections[1];
		}
		
		//adjust all clock corrections so that they remain within the bounds of the outputs
		E_BiasGroup biasGroup = E_BiasGroup::_from_integral(Sat.biasGroup());
		if (commonClockOffsetsMap[biasGroup][0] == 0)
		{
			commonClockOffsetsMap[biasGroup][0] = ssrClk.dclk[0];
			commonClockOffsetsMap[biasGroup][1] = ssrClk.dclk[1];
			commonClockEpochsMap [biasGroup][0] = ssrClkInput.vals[0].time;
			commonClockEpochsMap [biasGroup][1] = ssrClkInput.vals[1].time;
		}
		if	( ssrClkInput.vals[0].time != commonClockEpochsMap[biasGroup][0]
			||ssrClkInput.vals[1].time != commonClockEpochsMap[biasGroup][1])
		{
			continue; // commonClockOffset is lagging/leading this sat's clock, skip
		}
		ssrClk.dclk[0] -= commonClockOffsetsMap[biasGroup][0];
		ssrClk.dclk[1] -= commonClockOffsetsMap[biasGroup][1];
	}
}	


int RtcmEncoder::getUdiIndex(int udi)
{
	for (int i = 0; i < 16; i++) // 16 from updateInterval[16] above
	{
		if (updateInterval[i] == udi)
		{
			return i;
		}
	}
	
	BOOST_LOG_TRIVIAL(error) << "Error: udi is not valid :" << udi << ").";	
	
	return -1;
}

void RtcmEncoder::encodeWriteMessages(
	std::ostream& outputStream)
{
	if (outputStream)
	{
		outputStream.write((const char*) &data[0], data.size());
		data.clear();
	}
}

bool RtcmEncoder::encodeWriteMessageToBuffer(
	vector<uint8_t>& buffer)
{
	int i = 0;
	int messLength = buffer.size();
	
	if (buffer.empty())
	{
		return false;
	}
	
// 	unsigned char nbuf[messLength+6];
	vector<uint8_t> newbuffer(messLength+6);
	unsigned char* nbuf	= newbuffer.data();
	unsigned char* buf	= buffer.data();
	
	i = setbituInc(nbuf,i,8,	RTCM_PREAMBLE);
	i = setbituInc(nbuf,i,6,	0);
	i = setbituInc(nbuf,i,10,	messLength);

	memcpy(nbuf+3,buf,sizeof(uint8_t)*messLength);
	i = i + messLength*8;

	const unsigned char* bCrcBuf = (const unsigned char *)nbuf;
	unsigned int crcCalc = crc24q(bCrcBuf, sizeof(char)*(messLength+3));
	
	unsigned char* bCrcCalc = (unsigned char*)&crcCalc;
	unsigned int b1 = 0;
	unsigned int b2 = 0;
	unsigned int b3 = 0;
	setbituInc((unsigned char *)&b1,0,8,*(bCrcCalc+0));
	setbituInc((unsigned char *)&b2,0,8,*(bCrcCalc+1));
	setbituInc((unsigned char *)&b3,0,8,*(bCrcCalc+2));
	i = setbituInc(nbuf,i,8,b3);
	i = setbituInc(nbuf,i,8,b2);
	i = setbituInc(nbuf,i,8,b1);
	
	data.insert(data.end(), &nbuf[0], &nbuf[messLength+6]);
	
	return true;
}

vector<uint8_t> RtcmEncoder::encodeTimeStampRTCM()
{
	// Custom message code, for crcsi maximum length 4096 bits or 512 bytes.
	unsigned int messCode = +RtcmMessageType::CUSTOM;
	unsigned int messType = +E_RTCMSubmessage::TIMESTAMP;
	
	boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time();
	
	// Number of seconds since 1/1/1970, long is 64 bits and all may be used.
	long int seconds = (now - boost::posix_time::from_time_t(0)).total_seconds();
	
	//Number of fractional seconds, The largest this can be is 1000 which is 10 bits unsigned. 
	boost::posix_time::ptime now_mod_seconds	= boost::posix_time::from_time_t(seconds);
	auto subseconds	= now - now_mod_seconds;
	int milli_sec = subseconds.total_milliseconds();
	
	unsigned int* var = (unsigned int*) &seconds;
	
	int i = 0;
	//int byteLen = ceil((12.0+8.0+64.0+10.0)/8.0);
	int byteLen = 12;
	vector<uint8_t> buffer(byteLen);
	unsigned char* buf = buffer.data();
	i = setbituInc(buf, i, 12,		messCode);
	i = setbituInc(buf, i, 8,		messType);
	i = setbituInc(buf, i, 32,		var[0]);
	i = setbituInc(buf, i, 32,		var[1]);
	i = setbituInc(buf, i, 10, (int)milli_sec);
	
	return buffer;
}

vector<uint8_t> RtcmEncoder::encodeSsrComb(
	map<SatSys, SSROut>& ssrOutMap)
{
	int numSat = ssrOutMap.size();
	if (numSat == 0)
	{
		return vector<uint8_t>();
	}
	
	auto& [Sat, ssrOut1]	= *ssrOutMap.begin();
	auto& ssrEph			= ssrOut1.ssrEph;
	auto& ssrMeta			= ssrEph.ssrMeta;
	
	int i		= 0;
	int bitLen	= 0;
	int ni		= 0;
	unsigned int messCode = 0;
	if		(Sat.sys == +E_Sys::GPS) {	messCode = +RtcmMessageType::GPS_SSR_COMB_CORR;		ni= 8; bitLen = 68+numSat*205;}
	else if (Sat.sys == +E_Sys::GAL) {	messCode = +RtcmMessageType::GAL_SSR_COMB_CORR;		ni=10; bitLen = 68+numSat*207;}
	
	int byteLen = ceil(bitLen/8.0);
	vector<uint8_t> buffer(byteLen);
	unsigned char* buf = buffer.data();

	// Write the header information.
	i = setbituInc(buf,i,12,	messCode);
	i = setbituInc(buf,i,20,	ssrMeta.epochTime1s);
	i = setbituInc(buf,i,4,		ssrMeta.ssrUpdateIntIndex);
	i = setbituInc(buf,i,1,		ssrMeta.multipleMessage);
	i = setbituInc(buf,i,1,		ssrMeta.referenceDatum);
	i = setbituInc(buf,i,4,		ssrEph.iod);
	i = setbituInc(buf,i,16,	ssrMeta.provider);
	i = setbituInc(buf,i,4,		ssrMeta.solution);
	i = setbituInc(buf,i,6,		numSat);
	
	for (auto& [Sat, ssrOut] : ssrOutMap)
	{
		auto& ssrEph = ssrOut.ssrEph;
		auto& ssrClk = ssrOut.ssrClk;
			
		i = setbituInc(buf,i, 6,	Sat.prn);
		i = setbituInc(buf,i, ni,	ssrEph.iode);
		
		int d;
		d = (int)round(ssrEph.deph[0]		/ 0.1e-3);				i = setbitsInc(buf,i,22,d);
		d = (int)round(ssrEph.deph[1]		/ 0.4e-3);				i = setbitsInc(buf,i,20,d);
		d = (int)round(ssrEph.deph[2]		/ 0.4e-3);				i = setbitsInc(buf,i,20,d);
		d = (int)round(ssrEph.ddeph[0]		/ 0.001e-3);			i = setbitsInc(buf,i,21,d); 
		d = (int)round(ssrEph.ddeph[1]		/ 0.004e-3);			i = setbitsInc(buf,i,19,d);    
		d = (int)round(ssrEph.ddeph[2]		/ 0.004e-3);			i = setbitsInc(buf,i,19,d);
		
		d = (int)round(ssrClk.dclk[0]		/ 0.1e-3);				i = setbitsInc(buf,i,22,d); 
		d = (int)round(ssrClk.dclk[1]		/ 0.001e-3);			i = setbitsInc(buf,i,21,d);  
		d = (int)round(ssrClk.dclk[2]		/ 0.00002e-3);			i = setbitsInc(buf,i,27,d);   

		outputSsrEphToJson(ssrEph, Sat);
		outputSsrClkToJson(ssrClk, Sat);
	}
	
	int bitl = byteLen*8-i;
	if (bitl > 7 )
	{
		BOOST_LOG_TRIVIAL(error) << "Error encoding combined.\n";
		BOOST_LOG_TRIVIAL(error) << "Error: bitl : " << bitl << ", i : " << i << ", byteLen : " << byteLen << std::endl;
	}
	i = setbituInc(buf, i, bitl, 0); 
	
	return buffer;
}

vector<uint8_t> RtcmEncoder::encodeSsrPhase(
	SsrPBMap& ssrPBMap)
{
	int numSat = ssrPBMap.size();
	
	int i = 0;
	int bitLen = 0;

	int totalNbias = 0;
	for (auto& [Sat,ssrPhasBias] : ssrPBMap)
	{
		totalNbias += ssrPhasBias.obsCodeBiasMap.size();
	}
	
	if (totalNbias == 0)
		return vector<uint8_t>();
	
	// Write the header information.
	auto s_it = ssrPBMap.begin();
	auto& [Sat, ssrPhasBias] = *s_it;
	SSRMeta& ssrMeta = ssrPhasBias.ssrMeta;
	
	int np = 0;
	int ni = 0;
	int nj = 0;
	int offp = 0;
	unsigned int messCode = 0;
	if (Sat.sys == +E_Sys::GPS) {	messCode = +RtcmMessageType::GPS_SSR_PHASE_BIAS;	np=6; ni= 8; nj= 0; offp=0;	bitLen = 69+numSat*28+totalNbias*32;}	
	if (Sat.sys == +E_Sys::GAL)	{	messCode = +RtcmMessageType::GAL_SSR_PHASE_BIAS;	np=6; ni=10; nj= 0; offp=0;	bitLen = 69+numSat*28+totalNbias*32;}
	
	int byteLen = ceil(bitLen/8.0);
	vector<uint8_t> buffer(byteLen);
	unsigned char* buf = buffer.data();

	i = setbituInc(buf,i,12,messCode);
	i = setbituInc(buf,i,20,ssrMeta.epochTime1s);
	i = setbituInc(buf,i,4,	ssrMeta.ssrUpdateIntIndex);
	i = setbituInc(buf,i,1,	ssrMeta.multipleMessage);
	
	i = setbituInc(buf,i,4,	ssrPhasBias.iod);
	i = setbituInc(buf,i,16,ssrMeta.provider);
	i = setbituInc(buf,i,4,	ssrMeta.solution);

	i = setbituInc(buf,i,1,	ssrPhasBias.ssrPhase.dispBiasConistInd);
	i = setbituInc(buf,i,1,	ssrPhasBias.ssrPhase.MWConistInd); 
	
	i = setbituInc(buf,i,6,numSat);
	
	for (auto& [sat, ssrPhasBias] : ssrPBMap)
	{
		SSRPhase ssrPhase = ssrPhasBias.ssrPhase;
		
		int d;
															i = setbituInc(buf,i,np,sat.prn);
		d = ssrPhasBias.obsCodeBiasMap.size();				i = setbituInc(buf,i,5,	d);
		d = (int)round(ssrPhase.yawAngle	*256	/PI);	i = setbituInc(buf,i,9,	d);
		d = (int)round(ssrPhase.yawRate		*8192	/PI);	i = setbitsInc(buf,i,8,	d);
		
		for (auto& [obsCode, entry] : ssrPhasBias.obsCodeBiasMap)
		{
			SSRPhaseCh ssrPhaseCh = ssrPhasBias.ssrPhaseChs[obsCode];
			
			//BOOST_LOG_TRIVIAL(debug) << "Phase, obsCode : " << obsCode << std::endl;
			//BOOST_LOG_TRIVIAL(debug) << "E_sys         : " << sys << std::endl;
			//BOOST_LOG_TRIVIAL(debug) << "mCodes_gps.size() : " << mCodes_gps.size() << std::endl;
			//print_map( mCodes_gps.left, " E_ObsCode <--> RTCM ", BOOST_LOG_TRIVIAL(debug) );
			
			int rtcm_code = 0;
			if		( sat.sys == +E_Sys::GPS )		rtcm_code = mCodes_gps.left.at(obsCode);		//todo aaron, crash heaven, needs else, try
			else if ( sat.sys == +E_Sys::GAL )		rtcm_code = mCodes_gal.left.at(obsCode); 
			
			//BOOST_LOG_TRIVIAL(debug) << "rtcm_code      : " << rtcm_code << std::endl;
			
													i = setbituInc(buf,i,5,	rtcm_code);
													i = setbituInc(buf,i,1,	ssrPhaseCh.signalIntInd);
													i = setbituInc(buf,i,2,	ssrPhaseCh.signalWidIntInd);
													i = setbituInc(buf,i,4,	ssrPhaseCh.signalDisconCnt);
			d = (int)round(entry.bias / 0.0001);	i = setbitsInc(buf,i,20,d);
			
			traceSsrPhasB(sat, obsCode, ssrPhasBias);   
		}
	} 
	
	int bitl = byteLen*8-i;
	if (bitl > 7 )
	{
		BOOST_LOG_TRIVIAL(error) << "Error encoding SSR Phase.\n";
		BOOST_LOG_TRIVIAL(error) << "Error: bitl : " << bitl << ", i : " << i << ", byteLen : " << byteLen << std::endl;
	}
	
	i = setbituInc(buf,i,bitl,0);
	
	return buffer;
}


vector<uint8_t> RtcmEncoder::encodeSsrCode(
	SsrCBMap& ssrCBMap)
{	
	int numSat = ssrCBMap.size();
	
	int i = 0;

	int totalNbias = 0;
	for (auto s_it = ssrCBMap.begin(); s_it != ssrCBMap.end(); s_it++)
	{
		SSRCodeBias ssrCodeBias = s_it->second;
		totalNbias += ssrCodeBias.obsCodeBiasMap.size();
	}
	
	if (totalNbias == 0)
	{
		return vector<uint8_t>();
	}
	
	// Write the header information.

	auto s_it = ssrCBMap.begin();
	auto& [Sat, ssrCodeBias] = *s_it;
	SSRMeta& ssrMeta = ssrCodeBias.ssrMeta;
	
	int bitLen	= 67 
				+ 11 * numSat
				+ 19 * totalNbias;
	int byteLen = ceil(bitLen / 8.0);
	
	vector<uint8_t> buffer(byteLen);
	unsigned char* buf = buffer.data();
	
	unsigned int messCode = 0;
	if (Sat.sys == +E_Sys::GPS)	{	messCode = +RtcmMessageType::GPS_SSR_CODE_BIAS;}
	if (Sat.sys == +E_Sys::GAL)	{	messCode = +RtcmMessageType::GAL_SSR_CODE_BIAS;}

	i = setbituInc(buf,i,12,messCode);
	i = setbituInc(buf,i,20,ssrMeta.epochTime1s);
	i = setbituInc(buf,i,4,	ssrMeta.ssrUpdateIntIndex);
	i = setbituInc(buf,i,1,	ssrMeta.multipleMessage);
	
	i = setbituInc(buf,i,4,	ssrCodeBias.iod);
	i = setbituInc(buf,i,16,ssrMeta.provider);
	i = setbituInc(buf,i,4,	ssrMeta.solution);
	i = setbituInc(buf,i,6,	numSat);

	for (auto& [sat, ssrCodeBias] : ssrCBMap)
	{
		i = setbituInc(buf, i, 6, sat.prn);
		unsigned int nbias = ssrCodeBias.obsCodeBiasMap.size();

		i = setbituInc(buf, i, 5, nbias);
		
		for (auto& [obsCode, entry] : ssrCodeBias.obsCodeBiasMap)
		{
			int rtcm_code = 0;
			if		( sat.sys == +E_Sys::GPS )	{	rtcm_code = mCodes_gps.left.at(obsCode);		}
			else if ( sat.sys == +E_Sys::GAL )	{	rtcm_code = mCodes_gal.left.at(obsCode);		}

													i = setbituInc(buf, i, 5,	rtcm_code);
			int d = (int)round(entry.bias / 0.01);	i = setbitsInc(buf, i, 14,	d);

			traceSsrCodeB(sat, obsCode, ssrCodeBias);         
		}
	}
	
	int bitl = byteLen*8-i;
	if (bitl > 7)
	{
		BOOST_LOG_TRIVIAL(error) << "Error encoding SSR Code.\n";
		BOOST_LOG_TRIVIAL(error) << "Error: bitl : " << bitl << ", i : " << i << ", byteLen : " << byteLen << std::endl;
	}
	
	i = setbituInc(buf,i,bitl,0);
	
	return buffer;
}
