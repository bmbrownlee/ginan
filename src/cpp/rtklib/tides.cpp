
// #pragma GCC optimize ("O0")

#include <math.h>

#include "streamTrace.hpp"
#include "acsConfig.hpp"
#include "constants.hpp"
#include "algebra.hpp"
#include "common.hpp"
#include "tides.hpp"
#include "erp.hpp"
#include "vmf3.h"

#define AS2R        (D2R/3600.0)    /* arc sec to radian */
#define GME         3.986004415E+14 /* earth gravitational constant */
#define GMS         1.327124E+20    /* sun gravitational constant */
#define GMM         4.902801E+12    /* moon gravitational constant */

/* function prototypes -------------------------------------------------------*/
#ifdef IERS_MODEL
int dehanttideinel_(double *xsta, int *year, int *mon, int *day,
						double *fhr, double *xsun, double *xmon,
						double *dxtide);
#endif

/* solar/lunar tides (ref [2] 7) ---------------------------------------------*/
#ifndef IERS_MODEL
void tide_pl(const double *eu, const double *rp, double GMp,
					const double *pos, double *dr)
{
	const double H3=0.292,L3=0.015;
	double r,ep[3],latp,lonp,p,K2,K3,a,H2,L2,dp,du,cosp,sinl,cosl;
	int i;

//     trace(4,"tide_pl : pos=%.3f %.3f\n",pos[0]*R2D,pos[1]*R2D);

	if ((r=norm(rp,3))<=0.0) return;

	for (i=0;i<3;i++) ep[i]=rp[i]/r;

	K2=GMp/GME*SQR(RE_WGS84)*SQR(RE_WGS84)/(r*r*r);
	K3=K2*RE_WGS84/r;
	latp=asin(ep[2]); lonp=atan2(ep[1],ep[0]);
	cosp=cos(latp); sinl=sin(pos[0]); cosl=cos(pos[0]);

	/* step1 in phase (degree 2) */
	p=(3.0*sinl*sinl-1.0)/2.0;
	H2=0.6078-0.0006*p;
	L2=0.0847+0.0002*p;
	a=dot(ep,eu,3);
	dp=K2*3.0*L2*a;
	du=K2*(H2*(1.5*a*a-0.5)-3.0*L2*a*a);

	/* step1 in phase (degree 3) */
	dp+=K3*L3*(7.5*a*a-1.5);
	du+=K3*(H3*(2.5*a*a*a-1.5*a)-L3*(7.5*a*a-1.5)*a);

	/* step1 out-of-phase (only radial) */
	du+=3.0/4.0*0.0025*K2*sin(2.0*latp)*sin(2.0*pos[0])*sin(pos[1]-lonp);
	du+=3.0/4.0*0.0022*K2*cosp*cosp*cosl*cosl*sin(2.0*(pos[1]-lonp));

	dr[0]=dp*ep[0]+du*eu[0];
	dr[1]=dp*ep[1]+du*eu[1];
	dr[2]=dp*ep[2]+du*eu[2];

//     trace(5,"tide_pl : dr=%.3f %.3f %.3f\n",dr[0],dr[1],dr[2]);
}
/* displacement by solid earth tide (ref [2] 7) ------------------------------*/
void tide_solid(
	const double *rsun,
	const double *rmoon,
	const double *pos,
	const double *E,
	double gmst,
	double *dr)
{
	double dr1[3],dr2[3],eu[3],du,dn,sinl,sin2l;

//     trace(3,"tide_solid: pos=%.3f %.3f opt=%d\n",pos[0]*R2D,pos[1]*R2D,opt);

	/* step1: time domain */
	eu[0]=E[2]; eu[1]=E[5]; eu[2]=E[8];
	tide_pl(eu,rsun, GMS,pos,dr1);
	tide_pl(eu,rmoon,GMM,pos,dr2);

	/* step2: frequency domain, only K1 radial */
	sin2l=sin(2.0*pos[0]);
	du=-0.012*sin2l*sin(gmst+pos[1]);

	dr[0]=dr1[0]+dr2[0]+du*E[2];
	dr[1]=dr1[1]+dr2[1]+du*E[5];
	dr[2]=dr1[2]+dr2[2]+du*E[8];

	/* eliminate permanent deformation */
//     if (opt&8) {			//todo aaron, never enabled? add back in?
//         sinl=sin(pos[0]);
//         du=0.1196*(1.5*sinl*sinl-0.5);
//         dn=0.0247*sin2l;
//         dr[0]+=du*E[2]+dn*E[1];
//         dr[1]+=du*E[5]+dn*E[4];
//         dr[2]+=du*E[8]+dn*E[7];
//     }
//     trace(5,"tide_solid: dr=%.3f %.3f %.3f\n",dr[0],dr[1],dr[2]);
}
#endif /* !IERS_MODEL */

/* displacement by ocean tide loading (ref [2] 7) ----------------------------*/
void tide_oload(GTime tut, const double *otlDisplacement, double *denu)
{
	const double args[][5]={
		{1.40519E-4, 2.0,-2.0, 0.0, 0.00},  /* M2 */
		{1.45444E-4, 0.0, 0.0, 0.0, 0.00},  /* S2 */
		{1.37880E-4, 2.0,-3.0, 1.0, 0.00},  /* N2 */
		{1.45842E-4, 2.0, 0.0, 0.0, 0.00},  /* K2 */
		{0.72921E-4, 1.0, 0.0, 0.0, 0.25},  /* K1 */
		{0.67598E-4, 1.0,-2.0, 0.0,-0.25},  /* O1 */
		{0.72523E-4,-1.0, 0.0, 0.0,-0.25},  /* P1 */
		{0.64959E-4, 1.0,-3.0, 1.0,-0.25},  /* Q1 */
		{0.53234E-5, 0.0, 2.0, 0.0, 0.00},  /* Mf */
		{0.26392E-5, 0.0, 1.0,-1.0, 0.00},  /* Mm */
		{0.03982E-5, 2.0, 0.0, 0.0, 0.00}   /* Ssa */
	};
	const double ep1975[]={1975,1,1,0,0,0};
	double ep[6],fday,days,t,t2,t3,a[5],ang,dp[3]={0};
	int i,j;

//     trace(3,"tide_oload:\n");

	/* angular argument: see subroutine arg.f for reference [1] */
	time2epoch(tut,ep);
	fday=ep[3]*3600.0+ep[4]*60.0+ep[5];
	ep[3]=ep[4]=ep[5]=0.0;
	days = (epoch2time(ep) - epoch2time(ep1975))/86400.0+1.0;
	t=(27392.500528+1.000000035*days)/36525.0;
	t2=t*t; t3=t2*t;

	a[0]=fday;
	a[1]=(279.69668+36000.768930485*t+3.03E-4*t2)*D2R; /* H0 */
	a[2]=(270.434358+481267.88314137*t-0.001133*t2+1.9E-6*t3)*D2R; /* S0 */
	a[3]=(334.329653+4069.0340329577*t-0.010325*t2-1.2E-5*t3)*D2R; /* P0 */
	a[4]=2.0*PI;

	/* displacements by 11 constituents */
	for (i=0;i<11;i++) 
	{
		ang=0.0;
		for (j=0;j<5;j++) ang	+= a[j]*args[i][j];
		for (j=0;j<3;j++) dp[j]	+= otlDisplacement[j+i*6]*cos(ang-otlDisplacement[j+3+i*6]*D2R);
	}
	denu[0]=-dp[1];
	denu[1]=-dp[2];
	denu[2]= dp[0];

//     trace(5,"tide_oload: denu=%.3f %.3f %.3f\n",denu[0],denu[1],denu[2]);
}
/* iers mean pole (ref [7] eq.7.25) ------------------------------------------*/
void iers_mean_pole(GTime tut, double *xp_bar, double *yp_bar)
{
	const double ep2000[]={2000,1,1,0,0,0};
	double y,y2,y3;

	y = (tut - epoch2time(ep2000))/86400.0/365.25;

	if (y<3653.0/365.25) { /* until 2010.0 */
		y2=y*y; y3=y2*y;
		*xp_bar= 55.974+1.8243*y+0.18413*y2+0.007024*y3; /* (mas) */
		*yp_bar=346.346+1.7896*y-0.10729*y2-0.000908*y3;
	}
	else { /* after 2010.0 */
		*xp_bar= 23.513+7.6141*y; /* (mas) */
		*yp_bar=358.891-0.6287*y;
	}
}
/* displacement by pole tide (ref [7] eq.7.26) --------------------------------*/
void tide_pole(
	GTime			tut,
	const double*	pos, 
	ERPValues&		erpv,
	double*			denu)
{
	double xp_bar,yp_bar,m1,m2,cosl,sinl;

//     trace(3,"tide_pole: pos=%.3f %.3f\n",pos[0]*R2D,pos[1]*R2D);

	/* iers mean pole (mas) */
	iers_mean_pole(tut,&xp_bar,&yp_bar);

	/* ref [7] eq.7.24 */
	m1= erpv.xp/AS2R-xp_bar*1E-3; /* (as) */
	m2=-erpv.yp/AS2R+yp_bar*1E-3;

	/* sin(2*theta) = sin(2*phi), cos(2*theta)=-cos(2*phi) */
	cosl=cos(pos[1]);
	sinl=sin(pos[1]);
	denu[0]=  9E-3*sin(pos[0])    *(m1*sinl-m2*cosl); /* de= Slambda (m) */
	denu[1]= -9E-3*cos(2.0*pos[0])*(m1*cosl+m2*sinl); /* dn=-Stheta  (m) */
	denu[2]=-33E-3*sin(2.0*pos[0])*(m1*cosl+m2*sinl); /* du= Sr      (m) */

//     trace(5,"tide_pole : denu=%.3f %.3f %.3f\n",denu[0],denu[1],denu[2]);
}
/* tidal displacement ----------------------------------------------------------
* displacements by earth tides
* args   : gtime_t tutc     I   time in utc
*          double *rr       I   site position (ecef) (m)
*          int    opt       I   options (or of the followings)
*                                 1: solid earth tide
*                                 2: ocean tide loading
*                                 4: pole tide
*                                 8: elimate permanent deformation
*          double *erp      I   earth rotation parameters (nullptr: not used)
*          double *odisp    I   ocean loading parameters  (nullptr: not used)
*                                 odisp[0+i*6]: consituent i amplitude radial(m)
*                                 odisp[1+i*6]: consituent i amplitude west  (m)
*                                 odisp[2+i*6]: consituent i amplitude south (m)
*                                 odisp[3+i*6]: consituent i phase radial  (deg)
*                                 odisp[4+i*6]: consituent i phase west    (deg)
*                                 odisp[5+i*6]: consituent i phase south   (deg)
*                                (i=0:M2,1:S2,2:N2,3:K2,4:K1,5:O1,6:P1,7:Q1,
*                                   8:Mf,9:Mm,10:Ssa)
*          double *dr       O   displacement by earth tides (ecef) (m)
* return : none
* notes  : see ref [1], [2] chap 7
*          see ref [4] 5.2.1, 5.2.2, 5.2.3
*          ver.2.4.0 does not use ocean loading and pole tide corrections
*-----------------------------------------------------------------------------*/
void tidedisp(
	Trace&			trace,
	GTime			tutc,
	Vector3d&		recPos,
	ERP&			erp,
	const double*	otlDisplacement,
	Vector3d&		dr,
	Vector3d*		solid_ptr,
	Vector3d*		otl_ptr,
	Vector3d*		pole_ptr)
{
	double pos[2],E[9],drt[3],denu[3],gmst;
	double ep[6];
	int lv = 3;
#ifdef IERS_MODEL
	double ep[6],fhr;
	int year,mon,day;
#endif

	time2epoch(utc2gpst(tutc),ep);
	double mjd = ymdhms2jd(ep) - JD2MJD;

	tracepdeex(3,trace,"tidedisp: tutc=%s\n", tutc.to_string(0).c_str());

	ERPValues erpv;
	geterp(erp, tutc, erpv);

	GTime tut = tutc + erpv.ut1_utc;

	dr = Vector3d::Zero();

	if (recPos.norm() <= 0)
		return;

	pos[0] = asin(recPos[2] / recPos.norm());		//todo aaron, use other function
	pos[1] = atan2(recPos[1], recPos[0]);
	xyz2enu(pos,E);

	if (acsConfig.model.tides.solid)
	{
		/* solid earth tides */

		/* sun and moon position in ecef */
		Vector3d rs;
		Vector3d rm;
		sunmoonpos(tutc, erpv, &rs, &rm, &gmst);

#ifdef IERS_MODEL
		time2epoch(tutc,ep);
		year= (int)ep[0];
		mon = (int)ep[1];
		day = (int)ep[2];
		fhr = ep[3] + ep[4] / 60 + ep[5] / 3600;

		/* call DEHANTTIDEINEL */
		dehanttideinel_((double*)rr, &year, &mon, &day, &fhr, rs, rm, drt);
#else
		tide_solid(rs.data(), rm.data(), pos, E, gmst, drt);
#endif
		
		for (int i = 0; i < 3; i++)
		{
			dr(i) += drt[i];
			
			if (solid_ptr)
			{
				solid_ptr->data()[i] = drt[i];
			}
		}

		tracepdeex(lv, trace," %.6f      tide (solid)         = %14.4f %14.4f %14.4f\n",mjd,drt[0],drt[1],drt[2]);
	}
	
	if	( acsConfig.model.tides.otl
		&&otlDisplacement)
	{
		/* ocean tide loading */
		tide_oload(tut, otlDisplacement, denu);
		matmul("TN", 3, 1, 3, 1, E, denu, 0, drt);

		for (int i = 0; i < 3; i++)
		{
			dr(i) += drt[i];
			
			if (otl_ptr)
			{
				otl_ptr->data()[i] = drt[i];
			}
		}

		tracepdeex(lv, trace, " %.6f      tide (ocean)         = %14.4f %14.4f %14.4f\n", mjd, drt[0], drt[1], drt[2]);
	}
	
	if 	(acsConfig.model.tides.pole)
	{
		/* pole tide */
		tide_pole(tut, pos, erpv, denu);
		matmul("TN", 3, 1, 3, 1, E, denu, 0, drt);

		for (int i = 0; i < 3; i++)
		{
			dr(i) += drt[i];
			
			if (pole_ptr)
			{
				pole_ptr->data()[i] = drt[i];
			}
		}

		tracepdeex(lv, trace, " %.6f      tide (pole)          = %14.4f %14.4f %14.4f\n",mjd,drt[0],drt[1],drt[2]);
	}

	tracepdeex(5,trace, "tidedisp: dr=%.3f %.3f %.3f\n",dr(0),dr(1),dr(2));
}
