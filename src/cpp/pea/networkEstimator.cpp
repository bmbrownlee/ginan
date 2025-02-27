
// #pragma GCC optimize ("O0")

#include <iostream>
#include <vector>

#include "networkEstimator.hpp"
#include "eigenIncluder.hpp"
#include "algebraTrace.hpp"
#include "streamTrace.hpp"
#include "navigation.hpp"
#include "testUtils.hpp"
#include "acsConfig.hpp"
#include "constants.hpp"
#include "station.hpp"
#include "algebra.hpp"
#include "common.hpp"
#include "orbits.hpp"
#include "enums.h"
#include "ppp.hpp"
#include "erp.hpp"



/** Remove ambiguity states from filter when they deemed old or bad
 * This effectively reinitialises them on the following epoch as a new state, and can be used for simple
 * resolution of cycle-slips
 */
void removeBadAmbiguities(
	Trace&				trace,				///< Trace to output to
	KFState&			kfState) 			///< Filter to remove states from
{
	for (auto [key, index] : kfState.kfIndexMap)
	{
		if (key.type != KF::AMBIGUITY)
		{
			continue;
		}
		
		if (key.rec_ptr == nullptr)
		{
			continue;
		}
		
		E_FType ft = (E_FType) key.num;
		
		auto& rec		= *key.rec_ptr;
		auto& satStat	= rec.satStatMap[key.Sat];
		auto& sigStat	= satStat.sigStatMap[ft];
		
		if (sigStat.netwPhaseOutageCount >= acsConfig.pppOpts.outage_reset_limit)
		{
			sigStat.netwPhaseOutageCount = 0;
			
			trace << std::endl << "Phase ambiguity removed due to long outage: "		<< key;
			
			kfState.removeState(key);
			continue;
		}
		
		if (sigStat.netwPhaseRejectCount >= acsConfig.pppOpts.phase_reject_limit)
		{
			sigStat.netwPhaseRejectCount = 0;
			
			trace << std::endl << "Phase ambiguity removed due to high reject count: "	<< key;
			
			kfState.removeState(key);
			
			InitialState init = initialStateFromConfig(acsConfig.getRecOpts("").ion);
			
			if (init.estimate == false)
			{
				KFKey kfKey = key;
				for (kfKey.num = 0; kfKey.num < NUM_FTYPES; kfKey.num++)
				{
					kfState.removeState(kfKey);
					continue;
				}
			}
		}
		
		if 	(  acsConfig.reinit_on_all_slips
			&& sigStat.slip.any
			&& (  (acsConfig.excludeSlip.LLI	&& satStat.sigStatMap[F1].slip.LLI)
				||(acsConfig.excludeSlip.GF		&& satStat.sigStatMap[F1].slip.GF)	
				||(acsConfig.excludeSlip.MW		&& satStat.sigStatMap[F1].slip.MW)	
				||(acsConfig.excludeSlip.EMW	&& satStat.sigStatMap[F1].slip.EMW)	
				||(acsConfig.excludeSlip.CJ		&& satStat.sigStatMap[F1].slip.CJ)	
				||(acsConfig.excludeSlip.SCDIA	&& satStat.sigStatMap[F1].slip.SCDIA)))
		{
			trace << std::endl << "Phase ambiguity removed due cycle slip detection: "	<< key;
			
			kfState.removeState(key);
			
			InitialState init = initialStateFromConfig(acsConfig.getRecOpts("").ion);
			
			if (init.estimate == false)
			{
				KFKey kfKey = key;
				for (kfKey.num = 0; kfKey.num < NUM_FTYPES; kfKey.num++)
				{
					kfState.removeState(kfKey);
					continue;
				}
			}
			
			continue;
		}
	}
}

/** Remove ambiguity states from filter when they deemed old or bad
 * This effectively reinitialises them on the following epoch as a new state, and can be used for simple
 * resolution of cycle-slips
 */
void removeBadIonospheres(
	Trace&				trace,				///< Trace to output to
	KFState&			kfState) 			///< Filter to remove states from
{
	for (auto [key, index] : kfState.kfIndexMap)
	{
		if (key.type != KF::IONO_STEC)
		{
			continue;
		}
		
		double state;
		double variance;
		kfState.getKFValue(key, state, &variance);
		
		if (variance > 10000)
		{
			trace << std::endl << "Ionosphere removed due to high variance: " << key;
			
			kfState.removeState(key);
		}
	}
}

void postFilterChecks(
	KFMeas&	kfMeas)
{
	for (int i = 0; i < kfMeas.V.rows(); i++)
	{
		resetPhaseSignalError	(kfMeas, i);
		resetPhaseSignalOutage	(kfMeas, i);
	}
}


Matrix3d stationEopPartials(
	Vector3d&	rRec)
{
	//compute partials and convert to units of MxS

	Matrix3d partials;
	auto& X = rRec(0);
	auto& Y = rRec(1);
	auto& Z = rRec(2);
	partials(0,0) = +Z * MAS2R;		//dx/dxp		= dx/dRotY
	partials(0,1) =  0;				//dy/dxp		= dy/dRotY
	partials(0,2) = -X * MAS2R;		//dz/dxp		= dz/dRotY

	partials(1,0) =  0;				//dx/dyp		= dx/dRotX
	partials(1,1) = -Z * MAS2R;		//dy/dyp		= dy/dRotX
	partials(1,2) = +Y * MAS2R;		//dz/dyp		= dz/dRotX

	partials(2,0) = +Y * MTS2R;		//dx/dut1		= dx/dRotZ
	partials(2,1) = -X * MTS2R;		//dy/dut1		= dy/dRotZ
	partials(2,2) =  0;				//dz/dut1		= dz/dRotZ

	return partials;
}

/** Check and correct clock jitter/wraparound
 */
void correctRecClocks(
	Trace&		trace,		///< Trace to output to
	KFState&	kfState,	///< Filter to correct clock estimates in
	Station*	refRec)		///< Reference clock to use as basis for adjusting others
{
	double wraparound_distance	= CLIGHT * 1e-3;
	double wraparound_tolerance	= CLIGHT * acsConfig.clock_wrap_threshold;
	
	for (auto& [key, index] : kfState.kfIndexMap)
	{
		if	( (key.type		!= KF::REC_SYS_BIAS)
			||(key.num		!= 0)
			||(key.rec_ptr	== nullptr))
		{
			continue;
		}

		auto& rec		= *key.rec_ptr;
 		auto& recOpts	= acsConfig.getRecOpts(key.str);

		double deltaBias		= rec.    sol.dtRec_m[0] 
								- refRec->sol.dtRec_m[0];
								
		double previousDelta	= rec.sol.deltaDt_net_old[0];

		double deltaDelta = deltaBias - previousDelta;

		InitialState init = initialStateFromConfig(acsConfig.getRecOpts("").clk_rate);
			
		if (init.estimate == false)
		{
			//get, modify and set the old bias in the state according to SPP estimates
			double oldBias = 0;
			kfState.getKFValue(key, oldBias);
			oldBias += deltaDelta;
			kfState.setKFValue(key, oldBias);
			trace << std::endl
			<< "Adjusting " << key.str
			<< " clock by " << deltaDelta;
		}
		else if	( (rec.sol.deltaDt_net_old[0] == 0)
				||( abs(deltaDelta) > wraparound_distance - wraparound_tolerance
				  &&abs(deltaDelta) < wraparound_distance + wraparound_tolerance))
		{
			//get, modify and set the old bias in the state
			double oldBias = 0;
			kfState.getKFValue(key, oldBias);
			if (deltaDelta > 0)	{	oldBias += wraparound_distance;	}
			else				{	oldBias -= wraparound_distance;	}
			kfState.setKFValue(key, oldBias);
		}

		//store this value here for next time
		rec.sol.deltaDt_net_old[0] = deltaBias;
	}
}

void incrementOutageCount(
	StationMap&		stations)
{
	//increment the outage count for all signals
	for (auto& [id, rec] : stations)
	{
		for (auto& [Sat,	satStat] : rec.satStatMap)
		for (auto& [ft,		sigStat] : satStat.sigStatMap)
		{
			sigStat.netwPhaseOutageCount++;
		}
	}
}

/** Estimates parameters for a network of stations simultaneously
 */
void networkEstimator(
	Trace&			trace,			///< Trace to output to
	StationMap&		stations,		///< List of stations containing observations for this epoch
	KFState&		kfState,		///< Kalman filter object containing the network state parameters
	GTime			time)			///< The time of the epoch
{
	TestStack ts(__FUNCTION__);

	BOOST_LOG_TRIVIAL(info) << "Network Estimation...";
	
	kfState.initFilterEpoch();
	
	removeBadAmbiguities(trace, kfState);
	
	incrementOutageCount(stations);

	//count the satellites common between receivers
	int total = 0;
	std::map<int, int> satCountMap;
	for (auto& [id, rec] : stations)
	{
		int count = 0;
		for (auto& obs : rec.obsList)
		{
			if (acsConfig.process_sys[obs.Sat.sys] == false)
			{
				continue;
			}

			satCountMap[obs.Sat]++;
			count++;
		}

		total+= count;
	}

	KFMeasEntryList		kfMeasEntryList;
	static Station*	refRec = nullptr;

	for (auto& [satId, satNav] : nav.satNavMap)
	{
		SatSys Sat;
		Sat.fromHash(satId);
		
		if (acsConfig.process_sys[Sat.sys] == false)
		{
			continue;
		}
		
		auto& satOpts = acsConfig.getSatOpts(Sat);
		
		InitialState init = initialStateFromConfig(satOpts.orb);
			
		if (init.estimate)
			orbPartials(trace, tsync, Sat, satNav.satPartialMat);	
	}

	for (auto& [id, rec]	: stations)
	for (auto& obs 			: rec.obsList)
	for (auto& [ft, sig] 	: obs.Sigs)
	{
		auto& satOpts = acsConfig.getSatOpts(obs.Sat);
		auto& recOpts = acsConfig.getRecOpts(rec.id);

		int count = satCountMap[obs.Sat];

		if	( (count < 2)
			||(obs.		exclude)
			||(satOpts.	exclude)
			||(recOpts.	exclude))
		{
			continue;
		}

		if (obs.Sat.sys	== +E_Sys::GAL)		{	if (ft != FTYPE_IF15)	{	continue;	}}
		else 								{	if (ft != FTYPE_IF12)	{	continue;	}}


		ObsKey obsKeyCode = {	obs.Sat, rec.id, "P", ft	};
		ObsKey obsKeyPhas = {	obs.Sat, rec.id, "L", ft	};

		KFMeasEntry	codeMeas(&kfState, obsKeyCode);
		KFMeasEntry	phasMeas(&kfState, obsKeyPhas);

		SatNav&		satNav	= *obs.satNav_ptr;
		SatStat&	satStat	= *obs.satStat_ptr;
		SigStat&	sigStat	= satStat.sigStatMap[ft];
		
		double codeAdjust = 0;
		double phasAdjust = 0;

		codeMeas.metaDataMap["obs_ptr"]	= &obs;
		phasMeas.metaDataMap["obs_ptr"]	= &obs;
		
		phasMeas.metaDataMap["PhaseRejectCount_ptr"] = &sigStat.netwPhaseRejectCount;
		phasMeas.metaDataMap["PhaseOutageCount_ptr"] = &sigStat.netwPhaseOutageCount;

		//initialise this rec/sat's design matrix

		//create some keys for use below
		//they determine which states the measurements are applied to
		//in some cases they are per receiver, some states are per rec/sat pair etc..
							//type					sat			receiver
		KFKey satClockKey		=	{KF::SAT_CLOCK,				obs.Sat};
		KFKey satClockRateKey	=	{KF::SAT_CLOCK_RATE,		obs.Sat};
		KFKey satClockRateGMKey	=	{KF::SAT_CLOCK_RATE_GM,		obs.Sat};
		KFKey ambiguityKey		=	{KF::AMBIGUITY,				obs.Sat,	rec.id, (short)ft,						"", &rec	};
		KFKey refClockKey		=	{KF::REF_SYS_BIAS,			{},			rec.id,	SatSys(E_Sys::GPS).biasGroup(),	"", &rec	};
		KFKey recClockKey		=	{KF::REC_SYS_BIAS,			{},			rec.id,	SatSys(E_Sys::GPS).biasGroup(),	"", &rec	};
		KFKey recClockRateKey	=	{KF::REC_SYS_BIAS_RATE,		{},			rec.id,	SatSys(E_Sys::GPS).biasGroup(),	"", &rec	};
		KFKey recClockRateGMKey	=	{KF::REC_SYS_BIAS_RATE_GM,	{},			rec.id,	SatSys(E_Sys::GPS).biasGroup(),	"", &rec	};
		KFKey recSysBiasKey		=	{KF::REC_SYS_BIAS,			{},			rec.id, obs.Sat.biasGroup(),			"", &rec	};
		KFKey recPosKeys	[3];
		KFKey tropKeys		[3];
		KFKey tropGMKeys	[3];
		KFKey eopKeys		[3]		= {	{KF::EOP,				{},			xp_str},
										{KF::EOP,				{},			yp_str},
										{KF::EOP,				{},			ut1_str}	};
		KFKey eopRateKeys	[3]		= {	{KF::EOP_RATE,			{},			xp_str},
										{KF::EOP_RATE,			{},			yp_str},
										{KF::EOP_RATE,			{},			ut1_str}	};
		for (short i = 0; i < 3; i++)
		{
			recPosKeys[i]	= {KF::REC_POS,			{},			rec.id,	i,				"", &rec	};
		}
		for (short i = 0; i < 3; i++)
		{
			tropKeys[i]		= {KF::TROP,			{},			rec.id,	i,				"", &rec	};
		}
		for (short i = 0; i < 3; i++)
		{
			tropGMKeys[i]	= {KF::TROP_GM,			{},			rec.id,	i,				"", &rec	};
		}
		
		//add the elements in the design matrix.
		//if they use a state parameter that hasn't been used before, it will be created and initialised

		//all obs have GPS clock bias
// 		if (recOpts.clk.estimate)
		for (auto once : {1})
		{
			if	(    refRec					== nullptr
				&&( acsConfig.pivot_station	== rec.id
				  ||acsConfig.pivot_station	== "<AUTO>"))
			{
				//use this receiver as the reference receiver for clock offsets
				refRec = &rec;
				
				InitialState init		= {0, SQR(0.0001), 0};

				ObsKey obsKeyCode;
				obsKeyCode.type = "REF";
				
				KFMeasEntry	pseudoMeas1(&kfState, obsKeyCode);
				pseudoMeas1.setValue(0);
				pseudoMeas1.setNoise(0.000001);
				pseudoMeas1.addDsgnEntry(refClockKey,	+1,					init);
				kfMeasEntryList.push_back(pseudoMeas1);
				
				obsKeyCode.num += 1;
				KFMeasEntry	pseudoMeas2(&kfState, obsKeyCode);
				pseudoMeas2.setValue(0);
				pseudoMeas2.setNoise(0.000001);
				pseudoMeas2.addDsgnEntry(recSysBiasKey,	+1,					init);
				kfMeasEntryList.push_back(pseudoMeas2);
			}
			
			if (&rec != refRec)
			{
				InitialState init		= initialStateFromConfig(recOpts.clk);
			
				if (init.estimate == false)
				{
					continue;
				}
			
				codeMeas.addDsgnEntry(recClockKey,		+1,					init);
				phasMeas.addDsgnEntry(recClockKey,		+1,					init);

				for (auto once : {1})
				{
					InitialState recClkRateInit	= initialStateFromConfig(recOpts.clk_rate);
			
					if (recClkRateInit.estimate == false)
					{
						continue;
					}
			
					kfState.setKFTransRate(recClockKey, recClockRateKey,	1, recClkRateInit);
				}
				
				for (auto once : {1})
				{
					InitialState gmInit			= initialStateFromConfig(recOpts.clk_rate_gauss_markov);
			
					if (gmInit.estimate == false)
					{
						continue;
					}
			
					kfState.setKFTransRate(recClockKey, recClockRateGMKey,	1, gmInit);
				}
			
				// other systems may have inter-system bias too.
				if (obs.Sat.sys != +E_Sys::GPS)
				{
					InitialState init		= initialStateFromConfig(recOpts.clk);
					codeMeas.addDsgnEntry(recSysBiasKey,	+1,						init);
					phasMeas.addDsgnEntry(recSysBiasKey,	+1,						init);
				}
			}
		}

		
		for (int i = 0; i < 3; i++)
		{
			InitialState init		= initialStateFromConfig(recOpts.pos,						i);
			
			if (init.estimate == false)
			{
				continue;
			}
			
			codeMeas.addDsgnEntry(recPosKeys[i],	-satStat.e[i], 			init);
			phasMeas.addDsgnEntry(recPosKeys[i],	-satStat.e[i], 			init);
			if (rec.aprioriVar(i) == 0)
			{
				rec.aprioriVar(i) = sqrt(init.P);
			}
		}

// 		if (recOpts.trop.estimate)
		for (auto once : {1})
		{
			InitialState init		= initialStateFromConfig(recOpts.trop);
			
			if (init.estimate == false)
			{
				continue;
			}
			
			codeMeas.addDsgnEntry(tropKeys[0],		satStat.mapWet,			init);
			phasMeas.addDsgnEntry(tropKeys[0],		satStat.mapWet,			init);
		}

// 		if (recOpts.trop_gauss_markov.estimate)
		for (auto once : {1})
		{
			InitialState gmInit		= initialStateFromConfig(recOpts.trop_gauss_markov);
			
			if (gmInit.estimate == false)
			{
				continue;
			}
			
			codeMeas.addDsgnEntry(tropGMKeys[0],	satStat.mapWet,			gmInit);
			phasMeas.addDsgnEntry(tropGMKeys[0],	satStat.mapWet,			gmInit);
		}

// 		if (recOpts.trop_grads.estimate)
		for (int i = 0; i < 2; i++)
		{
			InitialState init		= initialStateFromConfig(recOpts.trop_grads,				i);
			
			if (init.estimate == false)
			{
				continue;
			}
			
			codeMeas.addDsgnEntry(tropKeys[i+1],	satStat.mapWetGrads[i],	init);
			phasMeas.addDsgnEntry(tropKeys[i+1],	satStat.mapWetGrads[i],	init);
		}

// 		if (recOpts.trop_grads_gauss_markov.estimate)
		for (int i = 0; i < 2; i++)
		{
			InitialState gmInit		= initialStateFromConfig(recOpts.trop_grads_gauss_markov,	i);
			
			if (gmInit.estimate == false)
			{
				continue;
			}
			
			codeMeas.addDsgnEntry(tropGMKeys[i+1],	satStat.mapWetGrads[i],	gmInit);
			phasMeas.addDsgnEntry(tropGMKeys[i+1],	satStat.mapWetGrads[i],	gmInit);
		}

// 		if (satOpts.clk.estimate)
		for (auto once : {1})
		{
			InitialState init		= initialStateFromConfig(satOpts.clk);
			
			if (init.estimate == false)
			{
				continue;
			}
			
			codeMeas.addDsgnEntry(satClockKey,		-1,						init);
			phasMeas.addDsgnEntry(satClockKey,		-1,						init);

			for (auto once : {1})
			{
				InitialState satClkRateInit	= initialStateFromConfig(satOpts.clk_rate);
			
				if (satClkRateInit.estimate == false)
				{
					continue;
				}
			
				kfState.setKFTransRate(satClockKey, satClockRateKey,	1,	satClkRateInit);
			}

			for (auto once : {1})
			{
				InitialState gmInit			= initialStateFromConfig(satOpts.clk_rate_gauss_markov);
			
				if (gmInit.estimate == false)
				{
					continue;
				}
			
				kfState.setKFTransRate(satClockKey, satClockRateGMKey,	1,	gmInit);
			}
		}

		for (auto once : {1})
		{
			InitialState init		= initialStateFromConfig(recOpts.amb, ft);
			
			if (init.estimate == false)
			{
				continue;
			}
		
			phasMeas.addDsgnEntry(ambiguityKey,		+1,						init);
		}

// 		if (satOpts.orb.estimate)
		{
// 			if (satNav.satOrbit.numUnknowns == orbitPartials.rows())
			for (int i = 0; i < satNav.satOrbit.numUnknowns; i++)
			{
				string name = satNav.satOrbit.parameterNames[i];
				KFKey orbPtKey	= {KF::ORBIT_PTS,	obs.Sat,	std::to_string(100 + i).substr(1) + "_" + name};

				InitialState init	= initialStateFromConfig(satOpts.orb, i);
				
				if (init.estimate == false)
				{
					continue;
				}
				
				VectorXd orbitPartials = satNav.satPartialMat * satStat.e;
				
				codeMeas.addDsgnEntry(orbPtKey,		orbitPartials(i),			init);
				phasMeas.addDsgnEntry(orbPtKey,		orbitPartials(i),			init);
			}
		}

// 		if (acsConfig.netwOpts.eop.estimate)
		{
			Matrix3d partialMatrix	= stationEopPartials(rec.aprioriPos);
			Vector3d eopPartials	= partialMatrix * satStat.e;

			ERPValues erpv[2];
			geterp(nav.erp, time,		erpv[0]);
			geterp(nav.erp, time + 1,	erpv[1]);

			for (int i = 0; i < 3; i++)
			{
				InitialState init	= initialStateFromConfig(acsConfig.pppOpts.eop,					i);
				
				if (init.estimate == false)
				{
					continue;
				}
				
				init.x = erpv[0].vals[i];
				
				if (i < 2)		init.x *= R2MAS;
				else			init.x *= S2MTS;
				
				codeAdjust += eopPartials(i) * init.x;
				phasAdjust += eopPartials(i) * init.x;

				codeMeas.addDsgnEntry(eopKeys[i],	eopPartials(i),				init);
				phasMeas.addDsgnEntry(eopKeys[i],	eopPartials(i),				init);
				
// 				if (acsConfig.netwOpts.eop_rates.estimate)
				for (auto once : {1})
				{
					InitialState eopRateInit	= initialStateFromConfig(acsConfig.pppOpts.eop_rates,	i);
					 					
					if (eopRateInit.estimate == false)
					{
						continue;
					}

					eopRateInit.x	= erpv[1].vals[i]
									- erpv[0].vals[i];
							
					if (i < 2)		eopRateInit.x *= R2MAS;
					else			eopRateInit.x *= S2MTS;
					
					eopRateInit.x *= 86400;
					
					kfState.setKFTransRate(eopKeys[i], eopRateKeys[i],	1/86400.0,	eopRateInit);
				}
			}
		}

		//initialise this rec/sat's measurements
		codeMeas.setValue(sig.codeRes + codeAdjust);
		phasMeas.setValue(sig.phasRes + phasAdjust);

		/* stochastic model (IF LC to be refined) */
		codeMeas.setNoise(sig.codeVar);
		phasMeas.setNoise(sig.phasVar);
		
		kfMeasEntryList.push_back(codeMeas);
		kfMeasEntryList.push_back(phasMeas);
	}

	//add process noise to existing states as per their initialisations.
	kfState.stateTransition(trace, time);

	//if not enough data is available, return early
	if (refRec == nullptr)
	{
		trace << std::endl << "No reference receiver found in network, skipping filter";
		return;
	}

	//combine the measurement list into a single matrix
	KFMeas combinedMeas = kfState.combineKFMeasList(kfMeasEntryList);
	combinedMeas.time = time;

	correctRecClocks(trace, kfState, refRec);

	//if there are uninitialised state values, estimate them using least squares

	if (kfState.lsqRequired)
	{
		kfState.lsqRequired = false;
		trace << std::endl << " -------INITIALISING NETWORK USING LEAST SQUARES--------" << std::endl;

 		kfState.leastSquareInitStates(trace, combinedMeas);
	}

	trace << std::endl << " -------DOING NETWORK KALMAN FILTER --------" << std::endl;

	kfState.filterKalman(trace, combinedMeas, false);
	
	postFilterChecks(combinedMeas);
}
