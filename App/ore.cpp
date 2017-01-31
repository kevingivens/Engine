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

#include <boost/algorithm/string.hpp>
#include <boost/timer.hpp>

#ifdef BOOST_MSVC
// disable warning C4503: '__LINE__Var': decorated name length exceeded, name was truncated
// This pragma statement needs to be at the top of the file - lower and it will not work:
// http://stackoverflow.com/questions/9673504/is-it-possible-to-disable-compiler-warning-c4503
// http://boost.2283326.n4.nabble.com/General-Warnings-and-pragmas-in-MSVC-td2587449.html
#pragma warning(disable : 4503)
#endif

#include <iostream>

#include <boost/filesystem.hpp>

#include <orea/orea.hpp>
#include <ored/ored.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/time/calendars/all.hpp>
#include <ql/time/daycounters/all.hpp>

#include "ore.hpp"

#ifdef BOOST_MSVC
#include <orea/auto_link.hpp>
#include <ored/auto_link.hpp>
#include <ql/auto_link.hpp>
#include <qle/auto_link.hpp>
// Find the name of the correct boost library with which to link.
#define BOOST_LIB_NAME boost_regex
#include <boost/config/auto_link.hpp>
#define BOOST_LIB_NAME boost_serialization
#include <boost/config/auto_link.hpp>
#define BOOST_LIB_NAME boost_date_time
#include <boost/config/auto_link.hpp>
#define BOOST_LIB_NAME boost_regex
#include <boost/config/auto_link.hpp>
#define BOOST_LIB_NAME boost_filesystem
#include <boost/config/auto_link.hpp>
#define BOOST_LIB_NAME boost_system
#include <boost/config/auto_link.hpp>
#endif

using namespace std;
using namespace ore::data;
using namespace ore::analytics;

int main(int argc, char** argv) {

    if (argc == 2 && (string(argv[1]) == "-v" || string(argv[1]) == "--version")) {
        cout << "ORE version " << OPEN_SOURCE_RISK_VERSION << endl;
        exit(0);
    }

    boost::timer timer;

    try {
        std::cout << "ORE starting" << std::endl;

        Size tab = 40;

        if (argc != 2) {
            std::cout << endl << "usage: ORE path/to/ore.xml" << endl << endl;
            return -1;
        }

        string inputFile(argv[1]);
        Parameters params;
        params.fromFile(inputFile);

        string outputPath = params.get("setup", "outputPath");
        string logFile = outputPath + "/" + params.get("setup", "logFile");
        Size logMask = 15; // Default level

        // Get log mask if available
        if (params.has("setup", "logMask")) {
            logMask = static_cast<Size>(parseInteger(params.get("setup", "logMask")));
        }

        boost::filesystem::path p{outputPath};
        if (!boost::filesystem::exists(p)) {
            boost::filesystem::create_directories(p);
        }
        QL_REQUIRE(boost::filesystem::is_directory(p), "output path '" << outputPath << "' is not a directory.");

        Log::instance().registerLogger(boost::make_shared<FileLogger>(logFile));
        Log::instance().setMask(logMask);
        Log::instance().switchOn();

        LOG("ORE starting");
        params.log();

        if (params.has("setup", "observationModel")) {
            string om = params.get("setup", "observationModel");
            ObservationMode::instance().setMode(om);
            LOG("Observation Mode is " << om);
        }

        string asofString = params.get("setup", "asofDate");
        Date asof = parseDate(asofString);
        Settings::instance().evaluationDate() = asof;

        ReportWriter rw;

        /*******************************
         * Market and fixing data loader
         */
        cout << setw(tab) << left << "Market data loader... " << flush;
        string inputPath = params.get("setup", "inputPath");
        string marketFile = inputPath + "/" + params.get("setup", "marketDataFile");
        string fixingFile = inputPath + "/" + params.get("setup", "fixingDataFile");
        string implyTodaysFixingsString = params.get("setup", "implyTodaysFixings");
        bool implyTodaysFixings = parseBool(implyTodaysFixingsString);
        CSVLoader loader(marketFile, fixingFile, implyTodaysFixings);
        cout << "OK" << endl;

        /*************
         * Conventions
         */
        cout << setw(tab) << left << "Conventions... " << flush;
        Conventions conventions;
        string conventionsFile = inputPath + "/" + params.get("setup", "conventionsFile");
        conventions.fromFile(conventionsFile);
        cout << "OK" << endl;

        /**********************
         * Curve configurations
         */
        cout << setw(tab) << left << "Curve configuration... " << flush;
        CurveConfigurations curveConfigs;
        string curveConfigFile = inputPath + "/" + params.get("setup", "curveConfigFile");
        curveConfigs.fromFile(curveConfigFile);
        cout << "OK" << endl;

        /*********
         * Markets
         */
        cout << setw(tab) << left << "Market... " << flush;
        TodaysMarketParameters marketParameters;
        string marketConfigFile = inputPath + "/" + params.get("setup", "marketConfigFile");
        marketParameters.fromFile(marketConfigFile);

        boost::shared_ptr<Market> market =
            boost::make_shared<TodaysMarket>(asof, marketParameters, loader, curveConfigs, conventions);
        cout << "OK" << endl;

        /************************
         * Pricing Engine Factory
         */
        cout << setw(tab) << left << "Engine factory... " << flush;
        boost::shared_ptr<EngineData> engineData = boost::make_shared<EngineData>();
        string pricingEnginesFile = inputPath + "/" + params.get("setup", "pricingEnginesFile");
        engineData->fromFile(pricingEnginesFile);

        map<MarketContext, string> configurations;
        configurations[MarketContext::irCalibration] = params.get("markets", "lgmcalibration");
        configurations[MarketContext::fxCalibration] = params.get("markets", "fxcalibration");
        configurations[MarketContext::pricing] = params.get("markets", "pricing");
        boost::shared_ptr<EngineFactory> factory =
            boost::make_shared<EngineFactory>(engineData, market, configurations);
        cout << "OK" << endl;

        /******************************
         * Load and Build the Portfolio
         */
        cout << setw(tab) << left << "Portfolio... " << flush;
        boost::shared_ptr<Portfolio> portfolio = boost::make_shared<Portfolio>();
        string portfolioFile = inputPath + "/" + params.get("setup", "portfolioFile");
        portfolio->load(portfolioFile);
        portfolio->build(factory);
        cout << "OK" << endl;

        /************
         * Curve dump
         */
        cout << setw(tab) << left << "Curve Report... " << flush;
        if (params.hasGroup("curves") && params.get("curves", "active") == "Y") {
            string curvesFile = outputPath + "/" + params.get("curves", "outputFileName");
            boost::shared_ptr<Report> curvesReport = boost::make_shared<CSVFileReport>(curvesFile);
            rw.writeCurves(params, marketParameters, market, curvesReport);
            cout << "OK" << endl;
        } else {
            LOG("skip curve report");
            cout << "SKIP" << endl;
        }

        /*********************
         * Portfolio valuation
         */
        cout << setw(tab) << left << "NPV Report... " << flush;
        if (params.hasGroup("npv") && params.get("npv", "active") == "Y") {
            string npvFile = outputPath + "/" + params.get("npv", "outputFileName");
            boost::shared_ptr<Report> npvReport = boost::make_shared<CSVFileReport>(npvFile);
            rw.writeNpv(params, market, params.get("markets", "pricing"), portfolio, npvReport);
            cout << "OK" << endl;
        } else {
            LOG("skip portfolio valuation");
            cout << "SKIP" << endl;
        }

        /**********************
         * Cash flow generation
         */
        cout << setw(tab) << left << "Cashflow Report... " << flush;
        if (params.hasGroup("cashflow") && params.get("cashflow", "active") == "Y") {
            string cashflowFile = outputPath + "/" + params.get("cashflow", "outputFileName");
            boost::shared_ptr<Report> cashflowReport = boost::make_shared<CSVFileReport>(cashflowFile);
            rw.writeCashflow(portfolio, cashflowReport);
            cout << "OK" << endl;
        } else {
            LOG("skip cashflow generation");
            cout << "SKIP" << endl;
        }

        /******************************************
         * Simulation: Scenario and Cube Generation
         */

        boost::shared_ptr<AggregationScenarioData> inMemoryScenarioData;
        boost::shared_ptr<NPVCube> inMemoryCube;
        Size cubeDepth = 0;

        if (params.hasGroup("simulation") && params.get("simulation", "active") == "Y") {

            cout << setw(tab) << left << "Simulation Setup... ";
            fflush(stdout);
            LOG("Build Simulation Model");
            string simulationConfigFile = inputPath + "/" + params.get("simulation", "simulationConfigFile");
            LOG("Load simulation model data from file: " << simulationConfigFile);
            boost::shared_ptr<CrossAssetModelData> modelData = boost::make_shared<CrossAssetModelData>();
            modelData->fromFile(simulationConfigFile);
            CrossAssetModelBuilder modelBuilder(market, params.get("markets", "lgmcalibration"),
                                                params.get("markets", "fxcalibration"),
                                                params.get("markets", "simulation"));
            boost::shared_ptr<QuantExt::CrossAssetModel> model = modelBuilder.build(modelData);

            LOG("Load Simulation Market Parameters");
            boost::shared_ptr<ScenarioSimMarketParameters> simMarketData(new ScenarioSimMarketParameters);
            simMarketData->fromFile(simulationConfigFile);

            LOG("Load Simulation Parameters");
            boost::shared_ptr<ScenarioGeneratorData> sgd(new ScenarioGeneratorData);
            sgd->fromFile(simulationConfigFile);
            ScenarioGeneratorBuilder sgb(sgd);
            boost::shared_ptr<ScenarioFactory> sf = boost::make_shared<SimpleScenarioFactory>();
            boost::shared_ptr<ScenarioGenerator> sg = sgb.build(
                model, sf, simMarketData, asof, market, params.get("markets", "simulation")); // pricing or simulation?

            // Optionally write out scenarios
            if (params.has("simulation", "scenariodump")) {
                string filename = outputPath + "/" + params.get("simulation", "scenariodump");
                sg = boost::make_shared<ScenarioWriter>(sg, filename);
            }

            boost::shared_ptr<ore::analytics::DateGrid> grid = sgd->grid();

            LOG("Build Simulation Market");
            boost::shared_ptr<ScenarioSimMarket> simMarket = boost::make_shared<ScenarioSimMarket>(
                sg, market, simMarketData, conventions, params.get("markets", "simulation"));

            LOG("Build engine factory for pricing under scenarios, linked to sim market");
            boost::shared_ptr<EngineData> simEngineData = boost::make_shared<EngineData>();
            string simPricingEnginesFile = inputPath + "/" + params.get("simulation", "pricingEnginesFile");
            simEngineData->fromFile(simPricingEnginesFile);
            map<MarketContext, string> configurations;
            configurations[MarketContext::irCalibration] = params.get("markets", "lgmcalibration");
            configurations[MarketContext::fxCalibration] = params.get("markets", "fxcalibration");
            configurations[MarketContext::pricing] = params.get("markets", "simulation");
            boost::shared_ptr<EngineFactory> simFactory =
                boost::make_shared<EngineFactory>(simEngineData, simMarket, configurations);

            LOG("Build portfolio linked to sim market");
            boost::shared_ptr<Portfolio> simPortfolio = boost::make_shared<Portfolio>();
            simPortfolio->load(portfolioFile);
            simPortfolio->build(simFactory);
            QL_REQUIRE(simPortfolio->size() == portfolio->size(),
                       "portfolio size mismatch, check simulation market setup");
            cout << "OK" << endl;

            LOG("Build valuation cube engine");
            Size samples = sgd->samples();
            string baseCurrency = params.get("simulation", "baseCurrency");
            if (params.has("simulation", "storeFlows") && params.get("simulation", "storeFlows") == "Y")
                cubeDepth = 2; // NPV and FLOW
            else
                cubeDepth = 1; // NPV only

            // Valuation calculators
            vector<boost::shared_ptr<ValuationCalculator>> calculators;
            calculators.push_back(boost::make_shared<NPVCalculator>(baseCurrency));
            if (cubeDepth > 1)
                calculators.push_back(boost::make_shared<CashflowCalculator>(baseCurrency, asof, grid, 1));
            ValuationEngine engine(asof, grid, simMarket);

            ostringstream o;
            o << "Aggregation Scenario Data " << grid->size() << " x " << samples << "... ";
            cout << setw(tab) << o.str() << flush;
            inMemoryScenarioData = boost::make_shared<InMemoryAggregationScenarioData>(grid->size(), samples);
            // Set AggregationScenarioData
            simMarket->aggregationScenarioData() = inMemoryScenarioData;
            cout << "OK" << endl;

            o.str("");
            o << "Build Cube " << simPortfolio->size() << " x " << grid->size() << " x " << samples << "... ";
            LOG("Build cube");
            auto progressBar = boost::make_shared<SimpleProgressBar>(o.str(), tab);
            auto progressLog = boost::make_shared<ProgressLog>("Building cube...");
            engine.registerProgressIndicator(progressBar);
            engine.registerProgressIndicator(progressLog);
            if (cubeDepth == 1)
                inMemoryCube =
                    boost::make_shared<SinglePrecisionInMemoryCube>(asof, simPortfolio->ids(), grid->dates(), samples);
            else if (cubeDepth == 2)
                inMemoryCube = boost::make_shared<SinglePrecisionInMemoryCubeN>(asof, simPortfolio->ids(),
                                                                                grid->dates(), samples, cubeDepth);
            else {
                QL_FAIL("cube depth 1 or 2 expected");
            }

            engine.buildCube(simPortfolio, inMemoryCube, calculators);
            cout << "OK" << endl;

            cout << setw(tab) << left << "Write Cube... " << flush;
            LOG("Write cube");
            if (params.has("simulation", "cubeFile")) {
                string cubeFileName = outputPath + "/" + params.get("simulation", "cubeFile");
                inMemoryCube->save(cubeFileName);
                cout << "OK" << endl;
            } else
                cout << "SKIP" << endl;

            cout << setw(tab) << left << "Write Aggregation Scenario Data... " << flush;
            LOG("Write scenario data");
            if (params.has("simulation", "additionalScenarioDataFileName")) {
                string outputFileNameAddScenData =
                    outputPath + "/" + params.get("simulation", "additionalScenarioDataFileName");
                inMemoryScenarioData->save(outputFileNameAddScenData);
                cout << "OK" << endl;
            } else
                cout << "SKIP" << endl;
        } else {
            LOG("skip simulation");
            cout << setw(tab) << left << "Simulation... ";
            cout << "SKIP" << endl;
        }

        /*****************************
         * Aggregation and XVA Reports
         */
        cout << setw(tab) << left << "Aggregation and XVA Reports... " << flush;
        if (params.hasGroup("xva") && params.get("xva", "active") == "Y") {

            // We reset this here because the date grid building below depends on it.
            Settings::instance().evaluationDate() = asof;

            string csaFile = inputPath + "/" + params.get("xva", "csaFile");
            boost::shared_ptr<NettingSetManager> netting = boost::make_shared<NettingSetManager>();
            netting->fromFile(csaFile);

            map<string, bool> analytics;
            analytics["exerciseNextBreak"] = parseBool(params.get("xva", "exerciseNextBreak"));
            analytics["exposureProfiles"] = parseBool(params.get("xva", "exposureProfiles"));
            analytics["cva"] = parseBool(params.get("xva", "cva"));
            analytics["dva"] = parseBool(params.get("xva", "dva"));
            analytics["fva"] = parseBool(params.get("xva", "fva"));
            analytics["colva"] = parseBool(params.get("xva", "colva"));
            analytics["collateralFloor"] = parseBool(params.get("xva", "collateralFloor"));
            if (params.has("xva", "mva"))
                analytics["mva"] = parseBool(params.get("xva", "mva"));
            else
                analytics["mva"] = false;
            if (params.has("xva", "dim"))
                analytics["dim"] = parseBool(params.get("xva", "dim"));
            else
                analytics["dim"] = false;

            boost::shared_ptr<NPVCube> cube;
            if (inMemoryCube)
                cube = inMemoryCube;
            else {
                Size cubeDepth = 1;
                if (params.has("xva", "hyperCube"))
                    cubeDepth = parseBool(params.get("xva", "hyperCube")) ? 2 : 1;

                if (cubeDepth > 1)
                    cube = boost::make_shared<SinglePrecisionInMemoryCubeN>();
                else
                    cube = boost::make_shared<SinglePrecisionInMemoryCube>();
                string cubeFile = outputPath + "/" + params.get("xva", "cubeFile");
                LOG("Load cube from file " << cubeFile);
                cube->load(cubeFile);
                LOG("Cube loading done");
            }

            QL_REQUIRE(cube->numIds() == portfolio->size(), "cube x dimension (" << cube->numIds()
                                                                                 << ") does not match portfolio size ("
                                                                                 << portfolio->size() << ")");

            boost::shared_ptr<AggregationScenarioData> scenarioData;
            if (inMemoryScenarioData)
                scenarioData = inMemoryScenarioData;
            else {
                scenarioData = boost::make_shared<InMemoryAggregationScenarioData>();
                string scenarioFile = outputPath + "/" + params.get("xva", "scenarioFile");
                scenarioData->load(scenarioFile);
            }

            QL_REQUIRE(scenarioData->dimDates() == cube->dates().size(), "scenario dates do not match cube grid size");
            QL_REQUIRE(scenarioData->dimSamples() == cube->samples(),
                       "scenario sample size does not match cube sample size");

            string baseCurrency = params.get("xva", "baseCurrency");
            string calculationType = params.get("xva", "calculationType");
            string allocationMethod = params.get("xva", "allocationMethod");
            Real marginalAllocationLimit = parseReal(params.get("xva", "marginalAllocationLimit"));
            Real quantile = parseReal(params.get("xva", "quantile"));
            string dvaName = params.get("xva", "dvaName");
            string fvaLendingCurve = params.get("xva", "fvaLendingCurve");
            string fvaBorrowingCurve = params.get("xva", "fvaBorrowingCurve");
            Real collateralSpread = parseReal(params.get("xva", "collateralSpread"));

            Real dimQuantile = 0.99;
            Size dimHorizonCalendarDays = 14;
            Size dimRegressionOrder = 0;
            vector<string> dimRegressors;
            Real dimScaling = 1.0;
            Size dimLocalRegressionEvaluations = 0;
            Real dimLocalRegressionBandwidth = 0.25;

            if (analytics["mva"] || analytics["dim"]) {
                dimQuantile = parseReal(params.get("xva", "dimQuantile"));
                dimHorizonCalendarDays = parseInteger(params.get("xva", "dimHorizonCalendarDays"));
                dimRegressionOrder = parseInteger(params.get("xva", "dimRegressionOrder"));
                string dimRegressorsString = params.get("xva", "dimRegressors");
                dimRegressors = parseListOfValues(dimRegressorsString);
                dimScaling = parseReal(params.get("xva", "dimScaling"));
                dimLocalRegressionEvaluations = parseInteger(params.get("xva", "dimLocalRegressionEvaluations"));
                dimLocalRegressionBandwidth = parseReal(params.get("xva", "dimLocalRegressionBandwidth"));
            }

            string marketConfiguration = params.get("markets", "simulation");

            boost::shared_ptr<PostProcess> postProcess = boost::make_shared<PostProcess>(
                portfolio, netting, market, marketConfiguration, cube, scenarioData, analytics, baseCurrency,
                allocationMethod, marginalAllocationLimit, quantile, calculationType, dvaName, fvaBorrowingCurve,
                fvaLendingCurve, collateralSpread, dimQuantile, dimHorizonCalendarDays, dimRegressionOrder,
                dimRegressors, dimLocalRegressionEvaluations, dimLocalRegressionBandwidth, dimScaling);

            for (auto t : postProcess->tradeIds()) {
                ostringstream o;
                o << outputPath << "/exposure_trade_" << t << ".csv";
                string tradeExposureFile = o.str();
                boost::shared_ptr<Report> tradeExposureReport = boost::make_shared<CSVFileReport>(tradeExposureFile);
                rw.writeTradeExposures(postProcess, tradeExposureReport, t);
            }
            for (auto n : postProcess->nettingSetIds()) {
                ostringstream o1;
                o1 << outputPath << "/exposure_nettingset_" << n << ".csv";
                string nettingSetExposureFile = o1.str();
                boost::shared_ptr<Report> nettingSetExposureReport =
                    boost::make_shared<CSVFileReport>(nettingSetExposureFile);
                rw.writeNettingSetExposures(postProcess, nettingSetExposureReport, n);

                ostringstream o2;
                o2 << outputPath << "/colva_nettingset_" << n << ".csv";
                string nettingSetColvaFile = o2.str();
                boost::shared_ptr<Report> nettingSetColvaReport =
                    boost::make_shared<CSVFileReport>(nettingSetColvaFile);
                rw.writeNettingSetColva(postProcess, nettingSetColvaReport, n);
            }

            string XvaFile = outputPath + "/xva.csv";
            boost::shared_ptr<Report> xvaReport = boost::make_shared<CSVFileReport>(XvaFile);
            rw.writeXVA(params, portfolio, postProcess, xvaReport);

            string rawCubeOutputFile = params.get("xva", "rawCubeOutputFile");
            CubeWriter cw1(outputPath + "/" + rawCubeOutputFile);
            map<string, string> nettingSetMap = portfolio->nettingSetMap();
            cw1.write(cube, nettingSetMap);

            string netCubeOutputFile = params.get("xva", "netCubeOutputFile");
            CubeWriter cw2(outputPath + "/" + netCubeOutputFile);
            cw2.write(postProcess->netCube(), nettingSetMap);

            if (analytics["dim"]) {
                string dimFile1 = outputPath + "/" + params.get("xva", "dimEvolutionFile");
                vector<string> dimFiles2;
                for (auto f : parseListOfValues(params.get("xva", "dimRegressionFiles")))
                    dimFiles2.push_back(outputPath + "/" + f);
                string nettingSet = params.get("xva", "dimOutputNettingSet");
                std::vector<Size> dimOutputGridPoints =
                    parseListOfValues<Size>(params.get("xva", "dimOutputGridPoints"), &parseInteger);
                postProcess->exportDimEvolution(dimFile1, nettingSet);
                postProcess->exportDimRegression(dimFiles2, nettingSet, dimOutputGridPoints);
            }

            cout << "OK" << endl;
        } else {
            LOG("skip XVA reports");
            cout << "SKIP" << endl;
        }

    } catch (std::exception& e) {
        ALOG("Error: " << e.what());
        cout << "Error: " << e.what() << endl;
    }

    cout << "run time: " << setprecision(2) << timer.elapsed() << " sec" << endl;
    cout << "ORE done." << endl;

    LOG("ORE done.");

    return 0;
}


