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

#include <ored/configuration/equitycurveconfig.hpp>
#include <ored/utilities/parsers.hpp>
#include <ored/utilities/to_string.hpp>
#include <ql/errors.hpp>

namespace ore {
namespace data {

EquityCurveConfig::EquityCurveConfig(const string& curveID, const string& curveDescription, const string& forecastingCurve, 
                                     const string& currency, const EquityCurveConfig::Type& type, const string& equitySpotQuote,
                                     const vector<string>& quotes, const string& dayCountID, bool extrapolation)
    : CurveConfig(curveID, curveDescription, quotes), forecastingCurve_(forecastingCurve), currency_(currency), type_(type),
      dayCountID_(dayCountID), extrapolation_(extrapolation) {
        quotes_ = quotes;
        quotes_.insert(quotes_.begin(), equitySpotQuote);
      }

void EquityCurveConfig::fromXML(XMLNode* node) {
    XMLUtils::checkNode(node, "EquityCurve");

    curveID_ = XMLUtils::getChildValue(node, "CurveId", true);
    curveDescription_ = XMLUtils::getChildValue(node, "CurveDescription", true);
    forecastingCurve_ = XMLUtils::getChildValue(node, "ForecastingCurve", true);
    currency_ = XMLUtils::getChildValue(node, "Currency", true);

    string type = XMLUtils::getChildValue(node, "Type", true);
    if (type == "DividendYield") {
        type_ = Type::DividendYield;
    } else if (type == "ForwardPrice") {
        type_ = Type::ForwardPrice;
    } else {
        QL_FAIL("Type " << type << " not recognized");
    }

    dayCountID_ = XMLUtils::getChildValue(node, "DayCounter", false);
    string equitySpotQuoteID = XMLUtils::getChildValue(node, "SpotQuote", true);
    quotes_ = XMLUtils::getChildrenValues(node, "Quotes", "Quote", true);
    quotes_.insert(quotes_.begin(), equitySpotQuoteID);

    extrapolation_ = XMLUtils::getChildValueAsBool(node, "Extrapolation"); // defaults to true
}

XMLNode* EquityCurveConfig::toXML(XMLDocument& doc) {
    XMLNode* node = doc.allocNode("EquityCurve");

    XMLUtils::addChild(doc, node, "CurveId", curveID_);
    XMLUtils::addChild(doc, node, "CurveDescription", curveDescription_);
    XMLUtils::addChild(doc, node, "ForecastingCurve", forecastingCurve_);
    XMLUtils::addChild(doc, node, "Currency", currency_);

    if (type_ == Type::DividendYield)
        XMLUtils::addChild(doc, node, "Type", "DividendYield");
    else if (type_ == Type::ForwardPrice)
        XMLUtils::addChild(doc, node, "Type", "ForwardPrice");
    else
        QL_FAIL("Unkown type in EquityCurveConfig::toXML()");

    XMLUtils::addChild(doc, node, "SpotQuote", equitySpotQuoteID());
    XMLUtils::addChild(doc, node, "DayCounter", dayCountID_);
    XMLUtils::addChildren(doc, node, "Quotes", "Quote", quotes_);
    XMLUtils::addChild(doc, node, "Extrapolation", extrapolation_);

    return node;
}
} // namespace data
} // namespace ore
