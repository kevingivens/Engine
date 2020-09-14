/*
 Copyright (C) 2016 Quaternion Risk Management Ltd
 All rights reserved.

 This file is part of ORE, a free-software/open-source library
 for transparent pricing and risk analysis - http://opensourcerisk.org

 ORE is free software: you can redistribute it and/or modify it
 under the terms of the Modified BSD License.  You should have received a
 copy of the license along with this program.
 The license is also available online at <http://opensourcerisk.org>

 This program is distributed on the basis that it will form a useful
 contribution to risk analytics and model standardisation, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE. See the license for more details.
*/

/*! \file orea/aggregation/postprocess.hpp
    \brief Exposure aggregation and XVA calculation
    \ingroup analytics
*/

#pragma once

#include <orea/aggregation/collatexposurehelper.hpp>
#include <orea/aggregation/dimcalculator.hpp>
#include <orea/cube/cubeinterpretation.hpp>
#include <orea/cube/inmemorycube.hpp>
#include <orea/scenario/aggregationscenariodata.hpp>

#include <ored/portfolio/nettingsetmanager.hpp>
#include <ored/portfolio/portfolio.hpp>
#include <ored/report/report.hpp>

#include <ql/time/date.hpp>

#include <boost/shared_ptr.hpp>

namespace ore {
namespace analytics {
using namespace QuantLib;
using namespace data;

enum class AllocationMethod {
    None,
    Marginal, // Pykhtin & Rosen, 2010
    RelativeFairValueGross,
    RelativeFairValueNet,
    RelativeXVA
};

std::ostream& operator<<(std::ostream& out, AllocationMethod m);

AllocationMethod parseAllocationMethod(const string& s);

//! Exposure Aggregation and XVA Calculation
/*!
  This class aggregates NPV cube data, computes exposure statistics
  and various XVAs, all at trade and netting set level:

  1) Exposures
  - Expected Positive Exposure, EPE: E[max(NPV(t),0) / N(t)]
  - Expected Negative Exposure, ENE: E[max(-NPV(t),0) / N(t)]
  - Basel Expected Exposure, EE_B: EPE(t)/P(t)
  - Basel Expected Positive Exposure, EPE_B
  - Basel Effective Expected Exposure, EEE_B: max( EEE_B(t-1), EE_B(t))
  - Basel Effective Expected Positive Exposure, EEPE_B
  - Potential Future Exposure, PFE: q-Quantile of the distribution of

  2) Dynamic Initial Margin via regression

  3) XVAs:
  - Credit Value Adjustment, CVA
  - Debit Value Adjustment, DVA
  - Funding Value Adjustment, FVA
  - Collateral Value Adjustment, COLVA
  - Margin Value Adjustment, MVA

  4) Allocation from netting set to trade level such that allocated contributions
  add up to the netting set
  - CVA and DVA
  - EPE and ENE

  All analytics are precomputed when the class constructor is called.
  A number of inspectors described below then return the individual analytics results.

  Note:
  - exposures are discounted at the numeraire N(t) used in the
  Monte Carlo simulation which produces the NPV cube.
  - NPVs take collateral into account, depending on CSA settings

  \ingroup analytics

  \todo Introduce enumeration for TradeAction type and owner
  \todo Interpolation for DIM(t-MPOR) when the simulation grid spacing is different from MPOR
  \todo Revise alternatives to the RelativeXVA exposure and XVA allocation method
  \todo Add trade-level MVA
  \todo Take the spread received on posted initial margin into account in MVA calculation
*/
class PostProcess {
public:
    //! Constructor
    PostProcess( //! Trade portfolio to identidy e.g. netting set, maturity, break dates for each trade
        const boost::shared_ptr<Portfolio>& portfolio,
        //! Netting set manager to access CSA details for each netting set
        const boost::shared_ptr<NettingSetManager>& nettingSetManager,
        //! Market data object to access e.g. discounting and funcing curves
        const boost::shared_ptr<Market>& market,
        //! Market configuration to use
        const std::string& configuration,
        //! Input NPV Cube
        const boost::shared_ptr<NPVCube>& cube,
        //! Subset of simulated market data, index fixings and FX spot rates, associated with the NPV cube
        const boost::shared_ptr<AggregationScenarioData>& scenarioData,
        //! Selection of analytics to be produced
        const map<string, bool>& analytics,
        //! Expression currency for all results
        const string& baseCurrency,
        //! Method to be used for Exposure/XVA allocation down to trade level
        const string& allocationMethod,
        //! Cutoff parameter for the marginal allocation method below which we switch to equal disctribution
        Real cvaMarginalAllocationLimit,
        //! Quantile for Potential Future Exposure output
        Real quantile = 0.95,
        //! Collateral calculation type to be used, see class %CollateralExposureHelper
        const string& calculationType = "Symmetric",
        //! Credit curve name to be used for "our" credit risk in DVA calculations
        const string& dvaName = "",
        //! Borrowing curve name to be used in FVA calculations
        const string& fvaBorrowingCurve = "",
        //! Lending curve name to be used in FVA calculations
        const string& fvaLendingCurve = "",
	//! Dynamic Initial Margin Calculator
	const boost::shared_ptr<DynamicInitialMarginCalculator>& dimCalculator = boost::shared_ptr<DynamicInitialMarginCalculator>(),
        //! Interpreter for cube storage (where to find which data items)
        const boost::shared_ptr<CubeInterpretation>& cubeInterpretation = boost::shared_ptr<CubeInterpretation>(),
        //! Assume t=0 collateral balance equals NPV (set to 0 if false)
        bool fullInitialCollateralisation = false,
	//! CVA spread sensitvitiy grid
	vector<Period> cvaSpreadSensiGrid = { 6*Months, 1*Years, 3*Years, 5*Years, 10*Years },
	//! CVA spread sensitivity shift size
	Real cvaSpreadSensiShiftSize = 0.0001,
        //! own capital discounting rate for discounting expected capital for KVA
        Real kvaCapitalDiscountRate = 0.10,
        //! alpha to adjust EEPE to give EAD for risk capital
        Real kvaAlpha = 1.4,
        //! regulatory adjustment, 1/min cap requirement
        Real kvaRegAdjustment = 12.5,
        //! Cost of Capital for KVA = regulatory adjustment x capital hurdle
        Real kvaCapitalHurdle = 0.012,
        //! Our KVA PD floor
        Real kvaOurPdFloor = 0.03,
        //! Their KVA PD floor
        Real kvaTheirPdFloor = 0.03,
        //! Our KVA CVA Risk Weight
        Real kvaOurCvaRiskWeight = 0.05,
        //! Their KVA CVA Risk Weight,
        Real kvaTheirCvaRiskWeight = 0.05);

    void setDimCalculator(boost::shared_ptr<DynamicInitialMarginCalculator> dimCalculator) {
        dimCalculator_ = dimCalculator;
    }

    const vector<Real>& spreadSensitivityTimes() { return cvaSpreadSensiTimes_; }
    const vector<Period>& spreadSensitivityGrid() { return cvaSpreadSensiGrid_; }
    
    //! Return list of Trade IDs in the portfolio
    const vector<string>& tradeIds() { return tradeIds_; }
    //! Return list of netting set IDs in the portfolio
    const vector<string>& nettingSetIds() { return nettingSetIds_; }
    //! Return the map of counterparty Ids
    const map<string, string>& counterpartyId() { return counterpartyId_; }

    //! Return trade level Expected Positive Exposure evolution
    const vector<Real>& tradeEPE(const string& tradeId);
    //! Return trade level Expected Negative Exposure evolution
    const vector<Real>& tradeENE(const string& tradeId);
    //! Return trade level Basel Expected Exposure evolution
    const vector<Real>& tradeEE_B(const string& tradeId);
    //! Return trade level Basel Expected Positive Exposure evolution
    const Real& tradeEPE_B(const string& tradeId);
    //! Return trade level Effective Expected Exposure evolution
    const vector<Real>& tradeEEE_B(const string& tradeId);
    //! Return trade level Effective Expected Positive Exposure evolution
    const Real& tradeEEPE_B(const string& tradeId);
    //! Return trade level Potential Future Exposure evolution
    const vector<Real>& tradePFE(const string& tradeId);
    // const vector<Real>& tradeVAR(const string& tradeId);

    //! Return Netting Set Expected Positive Exposure evolution
    const vector<Real>& netEPE(const string& nettingSetId);
    //! Return Netting Set Expected Negative Exposure evolution
    const vector<Real>& netENE(const string& nettingSetId);
    //! Return Netting Set Basel Expected Exposure evolution
    const vector<Real>& netEE_B(const string& nettingSetId);
    //! Return Netting Set Basel Expected Positive Exposure evolution
    const Real& netEPE_B(const string& nettingSetId);
    //! Return Netting Set Effective Expected Exposure evolution
    const vector<Real>& netEEE_B(const string& nettingSetId);
    //! Return Netting Set Effective Expected Positive Exposure evolution
    const Real& netEEPE_B(const string& nettingSetId);
    //! Return Netting Set Potential Future Exposure evolution
    const vector<Real>& netPFE(const string& nettingSetId);
    // const vector<Real>& netVAR(const string& nettingSetId);

    //! Return the netting set's expected collateral evolution
    const vector<Real>& expectedCollateral(const string& nettingSetId);
    //! Return the netting set's expected COLVA increments through time
    const vector<Real>& colvaIncrements(const string& nettingSetId);
    //! Return the netting set's expected Collateral Floor increments through time
    const vector<Real>& collateralFloorIncrements(const string& nettingSetId);

    //! Return the trade EPE, allocated down from the netting set level
    const vector<Real>& allocatedTradeEPE(const string& tradeId);
    //! Return trade ENE, allocated down from the netting set level
    const vector<Real>& allocatedTradeENE(const string& tradeId);
  
    //! Return Netting Set CVA Hazard Rate Sensitvity vector
    const vector<Real>& netCvaHazardRateSensitivity(const string& nettingSetId);
    //! Return Netting Set CVA Spread Sensitvity vector
    const vector<Real>& netCvaSpreadSensitivity(const string& nettingSetId);
    //! Return Netting Set CVA Spread Sensitvity vector
    const std::map<std::string, std::vector<QuantLib::Real>>& netCvaSpreadSensitivity() const { return netCvaSpreadSensi_; }

    //! Return trade (stand-alone) CVA
    Real tradeCVA(const string& tradeId);
    //! Return trade (stand-alone) DVA
    Real tradeDVA(const string& tradeId);
    //! Return trade (stand-alone) MVA
    Real tradeMVA(const string& tradeId);
    //! Return trade (stand-alone) FBA (Funding Benefit Adjustment)
    Real tradeFBA(const string& tradeId);
    //! Return trade (stand-alone) FCA (Funding Cost Adjustment)
    Real tradeFCA(const string& tradeId);
    //! Return trade (stand-alone) FBA (Funding Benefit Adjustment) excluding own survival probability
    Real tradeFBA_exOwnSP(const string& tradeId);
    //! Return trade (stand-alone) FCA (Funding Cost Adjustment) excluding own survival probability
    Real tradeFCA_exOwnSP(const string& tradeId);
    //! Return trade (stand-alone) FBA (Funding Benefit Adjustment) excluding both survival probabilities
    Real tradeFBA_exAllSP(const string& tradeId);
    //! Return trade (stand-alone) FCA (Funding Cost Adjustment) excluding both survival probabilities
    Real tradeFCA_exAllSP(const string& tradeId);
    //! Return allocated trade CVA (trade CVAs add up to netting set CVA)
    Real allocatedTradeCVA(const string& tradeId);
    //! Return allocated trade DVA (trade DVAs add up to netting set DVA)
    Real allocatedTradeDVA(const string& tradeId);
    //! Return netting set CVA
    Real nettingSetCVA(const string& nettingSetId);
    //! Return netting set DVA
    Real nettingSetDVA(const string& nettingSetId);
    //! Return netting set MVA
    Real nettingSetMVA(const string& nettingSetId);
    //! Return netting set FBA
    Real nettingSetFBA(const string& nettingSetId);
    //! Return netting set FCA
    Real nettingSetFCA(const string& nettingSetId);
    //! Return netting set KVA-CCR
    Real nettingSetOurKVACCR(const string& nettingSetId);
    //! Return netting set KVA-CCR from counterparty persepctive
    Real nettingSetTheirKVACCR(const string& nettingSetId);
    //! Return netting set KVA-CVA
    Real nettingSetOurKVACVA(const string& nettingSetId);
    //! Return netting set KVA-CVA from counterparty persepctive
    Real nettingSetTheirKVACVA(const string& nettingSetId);
    //! Return netting set FBA excluding own survival probability
    Real nettingSetFBA_exOwnSP(const string& nettingSetId);
    //! Return netting set FCA excluding own survival probability
    Real nettingSetFCA_exOwnSP(const string& nettingSetId);
    //! Return netting set FBA excluding both survival probabilities
    Real nettingSetFBA_exAllSP(const string& nettingSetId);
    //! Return netting set FCA excluding both survival probabilities
    Real nettingSetFCA_exAllSP(const string& nettingSetId);
    //! Return netting set COLVA
    Real nettingSetCOLVA(const string& nettingSetId);
    //! Return netting set Collateral Floor value
    Real nettingSetCollateralFloor(const string& nettingSetId);

    //! Inspector for the input NPV cube (by trade, time, scenario)
    const boost::shared_ptr<NPVCube>& cube() { return cube_; }
    //! Return the  for the input NPV cube after netting and collateral (by netting set, time, scenario)
    const boost::shared_ptr<NPVCube>& netCube() { return nettedCube_; }
    //! Return the dynamic initial margin cube (regression approach)
    //const boost::shared_ptr<NPVCube>& dimCube() { return dimCube_; }
    //! Write average (over samples) DIM evolution through time for all netting sets
    void exportDimEvolution(ore::data::Report& dimEvolutionReport);
    //! Write DIM as a function of sample netting set NPV for a given time step
    void exportDimRegression(const std::string& nettingSet, const std::vector<Size>& timeSteps,
                             const std::vector<boost::shared_ptr<ore::data::Report>>& dimRegReports);

    //! get the cvaSpreadSensiShiftSize
    QuantLib::Real cvaSpreadSensiShiftSize() { return cvaSpreadSensiShiftSize_; }
  
protected:
    //! Helper function to return the collateral account evolution for a given netting set
    boost::shared_ptr<vector<boost::shared_ptr<CollateralAccount>>>
    collateralPaths(const string& nettingSetId, const boost::shared_ptr<NettingSetManager>& nettingSetManager,
                    const boost::shared_ptr<Market>& market, const std::string& configuration,
                    const boost::shared_ptr<AggregationScenarioData>& scenarioData, Size dates, Size samples,
                    const vector<vector<Real>>& nettingSetValue, Real nettingSetValueToday,
                    const Date& nettingSetMaturity);

    void updateNettingSetKVA();
    void updateStandAloneXVA();
    void updateAllocatedXVA();

    boost::shared_ptr<Portfolio> portfolio_;
    boost::shared_ptr<NettingSetManager> nettingSetManager_;
    boost::shared_ptr<Market> market_;
    const std::string configuration_;
    boost::shared_ptr<NPVCube> cube_;
    boost::shared_ptr<AggregationScenarioData> scenarioData_;
    map<string, bool> analytics_;

    map<string, vector<Real>> tradeEPE_, tradeENE_, tradeEE_B_, tradeEEE_B_, tradePFE_, tradeVAR_;
    map<string, Real> tradeEPE_B_, tradeEEPE_B_;
    map<string, vector<Real>> allocatedTradeEPE_, allocatedTradeENE_;
    map<string, vector<Real>> netEPE_, netENE_, netEE_B_, netEEE_B_, netPFE_, netVAR_, expectedCollateral_;
    map<string, vector<Real>> netCvaHazardRateSensi_, netCvaSpreadSensi_;
    map<string, Real> netEPE_B_, netEEPE_B_;
    map<string, vector<Real>> colvaInc_, eoniaFloorInc_;
    map<string, Real> tradeCVA_, tradeDVA_, tradeMVA_, tradeFBA_, tradeFCA_, tradeFBA_exOwnSP_, tradeFCA_exOwnSP_,
        tradeFBA_exAllSP_, tradeFCA_exAllSP_;
    map<string, Real> sumTradeCVA_, sumTradeDVA_; // per netting set
    map<string, Real> allocatedTradeCVA_, allocatedTradeDVA_;
    map<string, Real> nettingSetCVA_, nettingSetDVA_, nettingSetMVA_;
    map<string, Real> nettingSetCOLVA_, nettingSetCollateralFloor_;
    map<string, Real> ourNettingSetKVACCR_, theirNettingSetKVACCR_, ourNettingSetKVACVA_, theirNettingSetKVACVA_;
    map<string, Real> nettingSetFCA_, nettingSetFBA_, nettingSetFCA_exOwnSP_, nettingSetFBA_exOwnSP_,
        nettingSetFCA_exAllSP_, nettingSetFBA_exAllSP_;
    boost::shared_ptr<NPVCube> nettedCube_;

    vector<string> tradeIds_;
    vector<string> nettingSetIds_;
    map<string, string> counterpartyId_; // for each nettingSetId
    string baseCurrency_;
    Real quantile_;
    CollateralExposureHelper::CalculationType calcType_;
    string dvaName_;
    string fvaBorrowingCurve_;
    string fvaLendingCurve_;
    boost::shared_ptr<DynamicInitialMarginCalculator> dimCalculator_;
    boost::shared_ptr<CubeInterpretation> cubeInterpretation_;
    bool fullInitialCollateralisation_;
    vector<Period> cvaSpreadSensiGrid_;
    vector<Time> cvaSpreadSensiTimes_;
    Real cvaSpreadSensiShiftSize_;
    Real kvaCapitalDiscountRate_;
    Real kvaAlpha_;
    Real kvaRegAdjustment_;
    Real kvaCapitalHurdle_;
    Real kvaOurPdFloor_;
    Real kvaTheirPdFloor_;
    Real kvaOurCvaRiskWeight_;
    Real kvaTheirCvaRiskWeight_;
};

} // namespace analytics
} // namespace ore
