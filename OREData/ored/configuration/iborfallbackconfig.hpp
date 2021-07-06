/*
 Copyright (C) 2021 Quaternion Risk Management Ltd
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

/*! \file ored/configuration/iborfallbackconfig.hpp
    \brief ibor fallback configuration
    \ingroup utilities
*/

#pragma once

#include <ored/utilities/xmlutils.hpp>

namespace ore {
namespace data {

class IborFallbackConfig : public XMLSerializable {
public:
    struct FallbackData {
        string rfrIndex;
        QuantLib::Real spread;
        QuantLib::Date switchDate;
    };

    IborFallbackConfig();
    IborFallbackConfig(const bool enableIborFallbacks, const bool useRfrCurveInTodaysMarket,
                       const bool useRfrCurveInSimulationMarket, const std::map<std::string, FallbackData>& fallbacks);

    bool enableIborFallbacks() const;
    bool useRfrCurveInTodaysMarket() const;
    bool useRfrCurveInSimulationMarket() const;

    void addIndexFallbackRule(const string& iborIndex, const FallbackData& fallbackData);

    bool isIndexReplaced(const string& iborIndex, const QuantLib::Date& asof = QuantLib::Date::maxDate()) const;
    const FallbackData& fallbackData(const string& iborIndex) const;

    void fromXML(XMLNode* node) override;
    XMLNode* toXML(XMLDocument& doc) override;

    void clear();
    static IborFallbackConfig defaultConfig();

private:
    bool enableIborFallbacks_;
    bool useRfrCurveInTodaysMarket_;
    bool useRfrCurveInSimulationMarket_;
    std::map<std::string, FallbackData> fallbacks_;
};

} // namespace data
} // namespace ore
