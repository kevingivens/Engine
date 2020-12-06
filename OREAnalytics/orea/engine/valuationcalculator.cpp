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

/*! \file engine/valuationcalculator.hpp
    \brief The cube valuation calculator interface
    \ingroup simulation
*/

#include <orea/engine/valuationcalculator.hpp>
#include <ored/portfolio/optionwrapper.hpp>
#include <ored/utilities/log.hpp>

namespace ore {
namespace analytics {

void NPVCalculator::calculate(const boost::shared_ptr<Trade>& trade, Size tradeIndex,
                              const boost::shared_ptr<SimMarket>& simMarket, boost::shared_ptr<NPVCube>& outputCube,
                              boost::shared_ptr<NPVCube>& outputCubeNettingSet, const Date& date, Size dateIndex,
                              Size sample, bool isCloseOut) {
    if (!isCloseOut)
        outputCube->set(npv(trade, simMarket), tradeIndex, dateIndex, sample, index_);
}

void NPVCalculator::calculateT0(const boost::shared_ptr<Trade>& trade, Size tradeIndex,
                                const boost::shared_ptr<SimMarket>& simMarket, boost::shared_ptr<NPVCube>& outputCube,
                                boost::shared_ptr<NPVCube>& outputCubeNettingSet) {
    outputCube->setT0(npv(trade, simMarket), tradeIndex, index_);
}

Real NPVCalculator::npv(const boost::shared_ptr<Trade>& trade, const boost::shared_ptr<SimMarket>& simMarket) {
    Real fx = simMarket->fxSpot(trade->npvCurrency() + baseCcyCode_)->value();
    Real numeraire = simMarket->numeraire();
    return trade->instrument()->NPV() * fx / numeraire;
}

void CashflowCalculator::calculate(const boost::shared_ptr<Trade>& trade, Size tradeIndex,
                                   const boost::shared_ptr<SimMarket>& simMarket,
                                   boost::shared_ptr<NPVCube>& outputCube,
                                   boost::shared_ptr<NPVCube>& outputCubeNettingSet, const Date& date, Size dateIndex,
                                   Size sample, bool isCloseOut) {
    if (isCloseOut)
        return;

    Real netFlow = 0;

    QL_REQUIRE(dateGrid_->dates()[dateIndex] == date, "Date mixup, date is " << date << " but grid index is "
                                                                             << dateIndex << ", grid(dateIndex) is "
                                                                             << dateGrid_->dates()[dateIndex]);
    // Date startDate = dateIndex == 0 ? t0Date_ : dateGrid_->dates()[dateIndex - 1];
    Date startDate = date;
    Date endDate = date == dateGrid_->dates().back() ? date : dateGrid_->dates()[dateIndex + 1];

    bool isOption = trade->instrument()->isOption();
    bool isExercised = false;
    bool isPhysical = false;
    Real longShort = 1.0;
    if (isOption) {
        boost::shared_ptr<data::OptionWrapper> wrapper =
            boost::dynamic_pointer_cast<data::OptionWrapper>(trade->instrument());
        isExercised = wrapper->isExercised();
        longShort = wrapper->isLong() ? 1.0 : -1.0;
        isPhysical = wrapper->isPhysicalDelivery();
    }

    try {
        if (!isOption || (isExercised && isPhysical)) {
            for (Size i = 0; i < trade->legs().size(); i++) {
                const Leg& leg = trade->legs()[i];
                Real legFlow = 0;
                for (auto flow : leg) {
                    // Take flows in (t, t+1]
                    if (startDate < flow->date() && flow->date() <= endDate)
                        legFlow += flow->amount();
                }
                if (legFlow != 0) {
                    // Do FX conversion and add to netFlow
                    Real fx = simMarket->fxSpot(trade->legCurrencies()[i] + baseCcyCode_)->value();
                    Real direction = trade->legPayers()[i] ? -1.0 : 1.0;
                    netFlow += legFlow * direction * longShort * fx;
                }
            }
        }
    } catch (std::exception& e) {
        ALOG("Failed to calculate cashflows for trade " << trade->id() << " : " << e.what());
        netFlow = 0;
    } catch (...) {
        ALOG("Failed to calculate cashflows for trade " << trade->id() << " : Unhandled Exception");
        netFlow = 0;
    }

    Real numeraire = simMarket->numeraire();

    outputCube->set(netFlow / numeraire, tradeIndex, dateIndex, sample, index_);
}

void NPVCalculatorFXT0::calculate(const boost::shared_ptr<Trade>& trade, Size tradeIndex,
                                  const boost::shared_ptr<SimMarket>& simMarket, boost::shared_ptr<NPVCube>& outputCube,
                                  boost::shared_ptr<NPVCube>& outputCubeNettingSet, const Date& date, Size dateIndex,
                                  Size sample, bool isCloseOut) {
    if (!isCloseOut)
        outputCube->set(npv(trade, simMarket), tradeIndex, dateIndex, sample, index_);
}

void NPVCalculatorFXT0::calculateT0(const boost::shared_ptr<Trade>& trade, Size tradeIndex,
                                    const boost::shared_ptr<SimMarket>& simMarket,
                                    boost::shared_ptr<NPVCube>& outputCube,
                                    boost::shared_ptr<NPVCube>& outputCubeNettingSet) {
    outputCube->setT0(npv(trade, simMarket), tradeIndex, index_);
}

Real NPVCalculatorFXT0::npv(const boost::shared_ptr<Trade>& trade, const boost::shared_ptr<SimMarket>& simMarket) {
    Real fx = 1.0;
    if (trade->npvCurrency() != baseCcyCode_)
        fx = t0Market_->fxSpot(trade->npvCurrency() + baseCcyCode_)->value();
    Real numeraire = simMarket->numeraire();
    return trade->instrument()->NPV() * fx / numeraire;
}

} // namespace analytics
} // namespace ore
