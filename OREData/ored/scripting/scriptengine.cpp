/*
 Copyright (C) 2019 Quaternion Risk Management Ltd
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

#include <ored/scripting/astresetter.hpp>
#include <ored/scripting/safestack.hpp>
#include <ored/scripting/scriptengine.hpp>
#include <ored/scripting/scriptparser.hpp>
#include <ored/scripting/utilities.hpp>

#include <ored/utilities/log.hpp>
#include <ored/utilities/parsers.hpp>
#include <ored/utilities/to_string.hpp>

#include <ql/errors.hpp>
#include <ql/indexes/indexmanager.hpp>

#include <boost/timer/timer.hpp>

#define TRACE(message, n)                                                                                              \
    {                                                                                                                  \
        if (interactive_) {                                                                                            \
            std::cerr << "\nScriptEngine: " << message << " at " << to_string((n).locationInfo)                        \
                      << "\nexpr value  = " << value.top() << "\ncurr filter = " << filter.top() << std::endl;         \
            std::cerr << printCodeContext(script_, &n);                                                                \
            std::string c;                                                                                             \
            do {                                                                                                       \
                std::cerr << "(c)ontext (q)uit ";                                                                      \
                std::getline(std::cin, c);                                                                             \
                if (c == "c")                                                                                          \
                    std::cerr << context_;                                                                             \
                else if (c == "q")                                                                                     \
                    interactive_ = false;                                                                              \
            } while (c == "c");                                                                                        \
        }                                                                                                              \
    }

namespace ore {
namespace data {

namespace {
class ASTRunner : public AcyclicVisitor,
                  public Visitor<ASTNode>,
                  public Visitor<OperatorPlusNode>,
                  public Visitor<OperatorMinusNode>,
                  public Visitor<OperatorMultiplyNode>,
                  public Visitor<OperatorDivideNode>,
                  public Visitor<NegateNode>,
                  public Visitor<FunctionAbsNode>,
                  public Visitor<FunctionExpNode>,
                  public Visitor<FunctionLogNode>,
                  public Visitor<FunctionSqrtNode>,
                  public Visitor<FunctionNormalCdfNode>,
                  public Visitor<FunctionNormalPdfNode>,
                  public Visitor<FunctionMinNode>,
                  public Visitor<FunctionMaxNode>,
                  public Visitor<FunctionPowNode>,
                  public Visitor<FunctionBlackNode>,
                  public Visitor<FunctionDcfNode>,
                  public Visitor<FunctionDaysNode>,
                  public Visitor<FunctionPayNode>,
                  public Visitor<FunctionLogPayNode>,
                  public Visitor<FunctionNpvNode>,
                  public Visitor<FunctionNpvMemNode>,
                  public Visitor<HistFixingNode>,
                  public Visitor<FunctionDiscountNode>,
                  public Visitor<FunctionFwdCompNode>,
                  public Visitor<FunctionFwdAvgNode>,
                  public Visitor<FunctionAboveProbNode>,
                  public Visitor<FunctionBelowProbNode>,
                  public Visitor<SortNode>,
                  public Visitor<PermuteNode>,
                  public Visitor<ConstantNumberNode>,
                  public Visitor<VariableNode>,
                  public Visitor<SizeOpNode>,
                  public Visitor<FunctionDateIndexNode>,
                  public Visitor<VarEvaluationNode>,
                  public Visitor<AssignmentNode>,
                  public Visitor<RequireNode>,
                  public Visitor<DeclarationNumberNode>,
                  public Visitor<SequenceNode>,
                  public Visitor<ConditionEqNode>,
                  public Visitor<ConditionNeqNode>,
                  public Visitor<ConditionLtNode>,
                  public Visitor<ConditionLeqNode>,
                  public Visitor<ConditionGtNode>,
                  public Visitor<ConditionGeqNode>,
                  public Visitor<ConditionNotNode>,
                  public Visitor<ConditionAndNode>,
                  public Visitor<ConditionOrNode>,
                  public Visitor<IfThenElseNode>,
                  public Visitor<LoopNode> {
public:
    ASTRunner(const boost::shared_ptr<Model> model, const std::string& script, bool& interactive, Context& context,
              ASTNode*& lastVisitedNode, boost::shared_ptr<PayLog> paylog)
        : model_(model), size_(model ? model->size() : 1), script_(script), interactive_(interactive), paylog_(paylog),
          context_(context), lastVisitedNode_(lastVisitedNode) {
        filter.emplace(size_, true);
        value.push(RandomVariable());
    }

    // helper functions to perform operations

    template <typename R>
    void binaryOp(ASTNode& n, const std::string& name, const std::function<R(ValueType, ValueType)>& op) {
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        checkpoint(n);
        auto right = value.pop();
        auto left = value.pop();
        value.push(op(left, right));
        TRACE(name << "( " << left << " , " << right << " )", n);
    }

    template <typename R> void unaryOp(ASTNode& n, const std::string& name, const std::function<R(ValueType)>& op) {
        n.args[0]->accept(*this);
        checkpoint(n);
        auto arg = value.pop();
        value.push(op(arg));
        TRACE(name << "( " << arg << " )", n);
    }

    // get ref to context variable + index (0 for scalars, 0,1,2,... for arrays)

    std::pair<ValueType&, long> getVariableRef(VariableNode& v) {
        checkpoint(v);
        if (v.isCached) {
            if (v.isScalar) {
                return std::make_pair(boost::ref(*v.cachedScalar), 0);
            } else {
                QL_REQUIRE(v.args[0], "array subscript required for variable '" << v.name << "'");
                v.args[0]->accept(*this);
                auto arg = value.pop();
                QL_REQUIRE(arg.which() == ValueTypeWhich::Number,
                           "array subscript must be of type NUMBER, got " << valueTypeLabels.at(arg.which()));
                RandomVariable i = boost::get<RandomVariable>(arg);
                QL_REQUIRE(i.deterministic(), "array subscript must be deterministic");
                long il = std::lround(i.at(0));
                QL_REQUIRE(static_cast<long>(v.cachedVector->size()) >= il && il >= 1,
                           "array index " << il << " out of bounds 1..." << v.cachedVector->size());
                return std::make_pair(boost::ref(v.cachedVector->operator[](il - 1)), il - 1);
            }
        } else {
            auto scalar = context_.scalars.find(v.name);
            if (scalar != context_.scalars.end()) {
                QL_REQUIRE(!v.args[0], "no array subscript allowed for variable '" << v.name << "'");
                v.isCached = true;
                v.isScalar = true;
                v.cachedScalar = &scalar->second;
                return std::make_pair(boost::ref(scalar->second), 0);
            }
            auto array = context_.arrays.find(v.name);
            if (array != context_.arrays.end()) {
                v.isCached = true;
                v.isScalar = false;
                v.cachedVector = &array->second;
                return getVariableRef(v);
            }
            QL_FAIL("variable '" << v.name << "' is not defined.");
        }
    }

    // helepr to declare a new context variable

    void declareVariable(const ASTNodePtr arg, const ValueType& val) {
        checkpoint(*arg);
        auto v = boost::dynamic_pointer_cast<VariableNode>(arg);
        QL_REQUIRE(v, "invalid declaration");
        if (context_.ignoreAssignments.find(v->name) != context_.ignoreAssignments.end()) {
            TRACE("declare(" << v->name << " ignored, because listed in ignoreAssignment variables set", *arg);
            return;
        }
        auto scalar = context_.scalars.find(v->name);
        auto array = context_.arrays.find(v->name);
        QL_REQUIRE(scalar == context_.scalars.end() && array == context_.arrays.end(),
                   "variable '" << v->name << "' already declared.");
        if (v->args[0]) {
            v->args[0]->accept(*this);
            checkpoint(*arg);
            auto size = value.pop();
            QL_REQUIRE(size.which() == ValueTypeWhich::Number, "expected NUMBER for array size definition");
            RandomVariable arraySize = boost::get<RandomVariable>(size);
            QL_REQUIRE(arraySize.deterministic(), "array size definition requires deterministic argument");
            long arraySizeL = std::lround(arraySize.at(0));
            QL_REQUIRE(arraySizeL >= 0, "expected non-negative array size, got " << arraySizeL);
            context_.arrays[v->name] = std::vector<ValueType>(arraySizeL, val);
            TRACE("declare(" << v->name << "[" << arraySizeL << "], " << val << ")", *arg);
        } else {
            context_.scalars[v->name] = val;
            TRACE("declare(" << v->name << ", " << val << ")", *arg);
        }
    }

    // record last visited node for diagnostics

    void checkpoint(ASTNode& n) { lastVisitedNode_ = &n; }

    // if we end up here, a node type is not handled

    void visit(ASTNode& n) override {
        checkpoint(n);
        QL_FAIL("unhandled node");
    }

    // operator / function node types

    void visit(OperatorPlusNode& n) override { binaryOp<ValueType>(n, "plus", operator+); }
    void visit(OperatorMinusNode& n) override {
        binaryOp<ValueType>(n, "minus", [](const ValueType& x, const ValueType& y) { return x - y; });
    }
    void visit(OperatorMultiplyNode& n) override { binaryOp<ValueType>(n, "multiply", operator*); }
    void visit(OperatorDivideNode& n) override { binaryOp<ValueType>(n, "divide", operator/); }
    void visit(NegateNode& n) override {
        unaryOp<ValueType>(n, "negate", [](const ValueType& x) { return -x; });
    }
    void visit(FunctionAbsNode& n) override { unaryOp<ValueType>(n, "abs", abs); }
    void visit(FunctionExpNode& n) override { unaryOp<ValueType>(n, "exp", exp); }
    void visit(FunctionLogNode& n) override { unaryOp<ValueType>(n, "log", log); }
    void visit(FunctionSqrtNode& n) override { unaryOp<ValueType>(n, "sqrt", sqrt); }
    void visit(FunctionNormalCdfNode& n) override { unaryOp<ValueType>(n, "normalCdf", normalCdf); }
    void visit(FunctionNormalPdfNode& n) override { unaryOp<ValueType>(n, "normalPdf", normalPdf); }
    void visit(FunctionMinNode& n) override { binaryOp<ValueType>(n, "min", min); }
    void visit(FunctionMaxNode& n) override { binaryOp<ValueType>(n, "max", max); }
    void visit(FunctionPowNode& n) override { binaryOp<ValueType>(n, "pow", pow); }

    // condition nodes

    void visit(ConditionEqNode& n) override { binaryOp<Filter>(n, "conditionEq", equal); }
    void visit(ConditionNeqNode& n) override { binaryOp<Filter>(n, "conditionNeq", notequal); }
    void visit(ConditionLtNode& n) override { binaryOp<Filter>(n, "conditionLt", lt); }
    void visit(ConditionLeqNode& n) override { binaryOp<Filter>(n, "conditionLeq", leq); }
    void visit(ConditionGeqNode& n) override { binaryOp<Filter>(n, "conditionGeq", geq); }
    void visit(ConditionGtNode& n) override { binaryOp<Filter>(n, "conditionGt", gt); }
    void visit(ConditionNotNode& n) override {
        unaryOp<Filter>(n, "conditionNot", [](const ValueType& x) { return logicalNot(x); });
    }

    void visit(ConditionAndNode& n) override {
        n.args[0]->accept(*this);
        auto left = value.pop();
        checkpoint(n);
        QL_REQUIRE(left.which() == ValueTypeWhich::Filter, "expected condition");
        Filter l = boost::get<Filter>(left);
        if (l.deterministic() && !l[0]) {
            // short cut if first expression is already false
            value.push(Filter(l.size(), false));
            TRACE("conditionAnd( false, ? )", n);
        } else {
            // no short cut possible
            n.args[1]->accept(*this);
            auto right = value.pop();
            checkpoint(n);
            value.push(logicalAnd(left, right));
            TRACE("conditionAnd( " << left << " , " << right << " )", n);
        }
    }

    void visit(ConditionOrNode& n) override {
        n.args[0]->accept(*this);
        auto left = value.pop();
        checkpoint(n);
        QL_REQUIRE(left.which() == ValueTypeWhich::Filter, "expected condition");
        Filter l = boost::get<Filter>(left);
        if (l.deterministic() && l[0]) {
            // short cut if first expression is already true
            value.push(Filter(l.size(), true));
            TRACE("conditionOr( true, ? )", n);
        } else {
            // no short cut possible
            n.args[1]->accept(*this);
            auto right = value.pop();
            checkpoint(n);
            value.push(logicalOr(left, right));
            TRACE("conditionOr( " << left << " , " << right << " )", n);
        }
    }

    // constants / variable related nodes

    void visit(ConstantNumberNode& n) override {
        checkpoint(n);
        value.push(RandomVariable(size_, n.value));
        TRACE("constantNumber( " << n.value << " )", n);
    }

    void visit(VariableNode& n) override {
        value.push(getVariableRef(n).first);
        checkpoint(n);
        TRACE("variable( " << n.name << " )", n);
    }

    void visit(DeclarationNumberNode& n) override {
        for (auto const& arg : n.args) {
            declareVariable(arg, RandomVariable(size_, 0.0));
            checkpoint(n);
        }
    }

    void visit(SizeOpNode& n) override {
        checkpoint(n);
        auto array = context_.arrays.find(n.name);
        if (array == context_.arrays.end()) {
            auto scalar = context_.scalars.find(n.name);
            if (scalar == context_.scalars.end())
                QL_FAIL("variable " << n.name << " is not defined");
            else
                QL_FAIL("SIZE can only be applied to array, " << n.name << " is a scalar");
        }
        value.push(RandomVariable(size_, static_cast<double>(array->second.size())));
        TRACE("size( " << n.name << " )", n);
    }

    void visit(FunctionDateIndexNode& n) override {
        checkpoint(n);
        auto array = context_.arrays.find(n.name);
        QL_REQUIRE(array != context_.arrays.end(),
                   "DATEINDEX: second argument event array '" << n.name << "' not found");
        auto v = boost::dynamic_pointer_cast<VariableNode>(n.args[0]);
        QL_REQUIRE(v, "DATEINDEX: first argument must be a variable expression");
        auto ref = getVariableRef(*v);
        checkpoint(n);
        QL_REQUIRE(ref.first.which() == ValueTypeWhich::Event, "DATEINDEX: first argument must be of type event");
        if (n.op == "EQ") {
            auto pos = std::find_if(array->second.begin(), array->second.end(),
                                    [&ref](const ValueType& v) { return ref.first == v; });
            if (pos == array->second.end())
                value.push(RandomVariable(size_, 0.0));
            else
                value.push(RandomVariable(size_, static_cast<double>(std::distance(array->second.begin(), pos) + 1)));
        } else if (n.op == "GEQ") {
            Size pos = std::lower_bound(array->second.begin(), array->second.end(), ref.first,
                                        [](const ValueType& l, const ValueType& r) -> bool {
                                            return boost::get<EventVec>(l).value < boost::get<EventVec>(r).value;
                                        }) -
                       array->second.begin() + 1;
            value.push(RandomVariable(size_, static_cast<double>(pos)));
        } else if (n.op == "GT") {
            Size pos = std::upper_bound(array->second.begin(), array->second.end(), ref.first,
                                        [](const ValueType& l, const ValueType& r) -> bool {
                                            return boost::get<EventVec>(l).value < boost::get<EventVec>(r).value;
                                        }) -
                       array->second.begin() + 1;
            value.push(RandomVariable(size_, static_cast<double>(pos)));
        } else {
            QL_FAIL("DATEINDEX: operation '" << n.op << "' not supported, expected EQ, GEQ, GT");
        }
        TRACE("dateindex( " << v->name << "[" << (ref.second + 1) << "] , " << n.name << " , " << n.op << " )", n);
    }

    void visit(AssignmentNode& n) override {
        n.args[1]->accept(*this);
        auto right = value.pop();
        checkpoint(n);
        auto v = boost::dynamic_pointer_cast<VariableNode>(n.args[0]);
        QL_REQUIRE(v, "expected variable identifier on LHS of assignment");
        if (context_.ignoreAssignments.find(v->name) != context_.ignoreAssignments.end()) {
            TRACE("assign(" << v->name << ") ignored, because variable is  listed in context's ignoreAssignment set",
                  n);
            return;
        }
        QL_REQUIRE(std::find(context_.constants.begin(), context_.constants.end(), v->name) == context_.constants.end(),
                   "can not assign to const variable '" << v->name << "'");
        auto ref = getVariableRef(*v);
        checkpoint(n);
        if (ref.first.which() == ValueTypeWhich::Event || ref.first.which() == ValueTypeWhich::Currency ||
            ref.first.which() == ValueTypeWhich::Index) {
            typeSafeAssign(ref.first, right);
        } else {
            QL_REQUIRE(ref.first.which() == ValueTypeWhich::Number,
                       "internal error: expected NUMBER, got " << valueTypeLabels.at(ref.first.which()));
            QL_REQUIRE(right.which() == ValueTypeWhich::Number, "invalid assignment: type "
                                                                    << valueTypeLabels.at(ref.first.which()) << " <- "
                                                                    << valueTypeLabels.at(right.which()));
            // TODO, better have a RESETTIME() function?
            boost::get<RandomVariable>(ref.first).setTime(Null<Real>());
            ref.first = conditionalResult(filter.top(), boost::get<RandomVariable>(right),
                                          boost::get<RandomVariable>(ref.first));
            boost::get<RandomVariable>(ref.first).updateDeterministic();
        }
        TRACE("assign( " << v->name << "[" << (ref.second + 1) << "] ) := " << ref.first << " ("
                         << valueTypeLabels.at(right.which()) << ") using filter " << filter.top(),
              n);
    }

    // require node

    void visit(RequireNode& n) override {
        n.args[0]->accept(*this);
        auto condition = value.pop();
        checkpoint(n);
        QL_REQUIRE(condition.which() == ValueTypeWhich::Filter, "expected condition");
        // check implication filter true => condition true
        auto c = !filter.top() || boost::get<Filter>(condition);
        c.updateDeterministic();
        QL_REQUIRE(c.deterministic() && c.at(0), "required condition is not (always) fulfilled");
        TRACE("require( " << condition << " ) for filter " << filter.top(), n);
    }

    // control flow nodes

    void visit(SequenceNode& n) override {
        TRACE("instruction_sequence()", n);
        for (auto const& arg : n.args) {
            arg->accept(*this);
            checkpoint(n);
        }
    }

    void visit(IfThenElseNode& n) override {
        n.args[0]->accept(*this);
        auto if_ = value.pop();
        checkpoint(n);
        QL_REQUIRE(if_.which() == ValueTypeWhich::Filter,
                   "IF must be followed by a boolean, got " << valueTypeLabels.at(if_.which()));
        Filter cond = boost::get<Filter>(if_);
        TRACE("if( " << cond << " )", n);
        Filter baseFilter = filter.top();
        Filter currentFilter = baseFilter && cond;
        currentFilter.updateDeterministic();
        filter.push(currentFilter);
        TRACE("then( filter = " << currentFilter << " )", n);
        if (!currentFilter.deterministic() || currentFilter[0]) {
            n.args[1]->accept(*this);
            checkpoint(n);
        }
        filter.pop();
        if (n.args[2]) {
            currentFilter = baseFilter && !cond;
            currentFilter.updateDeterministic();
            filter.push(currentFilter);
            TRACE("else( filter = " << currentFilter << ")", n);
            if (!currentFilter.deterministic() || currentFilter[0]) {
                n.args[2]->accept(*this);
                checkpoint(n);
            }
            filter.pop();
        }
    }

    void visit(LoopNode& n) override {
        checkpoint(n);
        auto var = context_.scalars.find(n.name);
        QL_REQUIRE(var != context_.scalars.end(), "loop variable '" << n.name << "' not defined or not scalar");
        QL_REQUIRE(std::find(context_.constants.begin(), context_.constants.end(), n.name) == context_.constants.end(),
                   "loop variable '" << n.name << "' is constant");
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        n.args[2]->accept(*this);
        auto step = value.pop();
        auto right = value.pop();
        auto left = value.pop();
        checkpoint(n);
        QL_REQUIRE(left.which() == ValueTypeWhich::Number && right.which() == ValueTypeWhich::Number &&
                       step.which() == ValueTypeWhich::Number,
                   "loop bounds and step must be of type NUMBER, got " << valueTypeLabels.at(left.which()) << ", "
                                                                       << valueTypeLabels.at(right.which()) << ", "
                                                                       << valueTypeLabels.at(step.which()));
        RandomVariable a = boost::get<RandomVariable>(left);
        RandomVariable b = boost::get<RandomVariable>(right);
        RandomVariable s = boost::get<RandomVariable>(step);
        QL_REQUIRE(a.deterministic(), "first loop bound must be deterministic");
        QL_REQUIRE(b.deterministic(), "second loop bound must be deterministic");
        QL_REQUIRE(s.deterministic(), "loop step must be deterministic");
        long al = std::lround(a.at(0));
        long bl = std::lround(b.at(0));
        long sl = std::lround(s.at(0));
        QL_REQUIRE(sl != 0, "loop step must be non-zero");
        long cl = al;
        while ((sl > 0 && cl <= bl) || (sl < 0 && cl >= bl)) {
            TRACE("for( " << n.name << " : " << cl << " (" << al << "," << bl << "))", n);
            var->second = RandomVariable(size_, static_cast<double>(cl));
            n.args[3]->accept(*this);
            checkpoint(n);
            QL_REQUIRE(var->second.which() == ValueTypeWhich::Number &&
                           close_enough_all(boost::get<RandomVariable>(var->second),
                                            RandomVariable(size_, static_cast<double>(cl))),
                       "loop variable was modified in body from " << cl << " to " << var->second
                                                                  << ", this is illegal.");
            cl += sl;
        }
    }

    // day counter functions

    void dayCounterFunctionHelper(ASTNode& n, DayCounter& daycounter, Date& date1, Date& date2) {
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        n.args[2]->accept(*this);
        checkpoint(n);

        auto d2 = value.pop();
        auto d1 = value.pop();
        auto dc = value.pop();

        QL_REQUIRE(dc.which() == ValueTypeWhich::Daycounter, "dc must be DAYCOUNTER");
        QL_REQUIRE(d1.which() == ValueTypeWhich::Event, "d1 must be EVENT");
        QL_REQUIRE(d2.which() == ValueTypeWhich::Event, "d2 must be EVENT");

        date1 = boost::get<EventVec>(d1).value;
        date2 = boost::get<EventVec>(d2).value;

        daycounter = ore::data::parseDayCounter(boost::get<DaycounterVec>(dc).value);
    }

    void visit(FunctionDcfNode& n) override {
        Date date1, date2;
        DayCounter daycounter;
        dayCounterFunctionHelper(n, daycounter, date1, date2);
        QL_REQUIRE(model_, "model is null");
        value.push(RandomVariable(model_->size(), daycounter.yearFraction(date1, date2)));
        TRACE("dcf( " << date1 << " , " << date2 << " )", n);
    }

    void visit(FunctionDaysNode& n) override {
        Date date1, date2;
        DayCounter daycounter;
        dayCounterFunctionHelper(n, daycounter, date1, date2);
        QL_REQUIRE(model_, "model is null");
        value.push(RandomVariable(model_->size(), static_cast<double>(daycounter.dayCount(date1, date2))));
        TRACE("days( " << date1 << " , " << date2 << " )", n);
    }

    // SORT and PERMUTE instructions

    void visit(SortNode& n) override {
        checkpoint(n);

        std::vector<RandomVariable*> x, y, p;

        if (n.args[0]) {
            auto xname = boost::dynamic_pointer_cast<VariableNode>(n.args[0]);
            QL_REQUIRE(xname, "x must be a variable");
            QL_REQUIRE(!xname->args[0], "x must not be indexed");
            auto xv = context_.arrays.find(xname->name);
            QL_REQUIRE(xv != context_.arrays.end(), "did not find array with name '" << xname->name << "'");
            for (Size c = 0; c < xv->second.size(); ++c) {
                QL_REQUIRE(xv->second[c].which() == ValueTypeWhich::Number, "x must be NUMBER");
                x.push_back(&boost::get<RandomVariable>(xv->second[c]));
            }
        }

        if (n.args[1]) {
            auto yname = boost::dynamic_pointer_cast<VariableNode>(n.args[1]);
            QL_REQUIRE(yname, "y must be a variable");
            QL_REQUIRE(!yname->args[0], "y must not be indexed");
            auto yv = context_.arrays.find(yname->name);
            QL_REQUIRE(yv != context_.arrays.end(), "did not find array with name '" << yname->name << "'");
            for (Size c = 0; c < yv->second.size(); ++c) {
                QL_REQUIRE(yv->second[c].which() == ValueTypeWhich::Number, "y must be NUMBER");
                y.push_back(&boost::get<RandomVariable>(yv->second[c]));
            }
        }

        if (n.args[2]) {
            auto pname = boost::dynamic_pointer_cast<VariableNode>(n.args[2]);
            QL_REQUIRE(pname, "p must be a variable");
            QL_REQUIRE(!pname->args[0], "p must not be indexed");
            auto pv = context_.arrays.find(pname->name);
            QL_REQUIRE(pv != context_.arrays.end(), "did not find array with name '" << pname->name << "'");
            for (Size c = 0; c < pv->second.size(); ++c) {
                QL_REQUIRE(pv->second[c].which() == ValueTypeWhich::Number, "p must be NUMBER");
                p.push_back(&boost::get<RandomVariable>(pv->second[c]));
            }
        }

        // set y to target (may be x itself)

        if (y.empty())
            y = x;

        QL_REQUIRE(x.size() >= 1, "array size must be >= 1");
        QL_REQUIRE(y.size() == x.size(),
                   "y array size (" << y.size() << ") must match x array size (" << x.size() << ")");
        QL_REQUIRE(p.empty() || p.size() == x.size(),
                   "p array size (" << p.size() << ") must match x array size (" << p.size() << ")");

        for (Size c = 0; c < x.size(); ++c) {
            QL_REQUIRE(x[c]->size() == y[c]->size(), "x[" << c << "] size (" << x[c]->size() << ") must match y[" << c
                                                          << "] size (" << y[c]->size() << ")");
            QL_REQUIRE(p.empty() || x[c]->size() == p[c]->size(), "x[" << c << "] size (" << x[c]->size()
                                                                       << ") must match i[" << c << "] size ("
                                                                       << p[c]->size() << ")");
        }

        const Filter& flt = filter.top();
        QL_REQUIRE(flt.size() == x[0]->size(),
                   "filter has size " << flt.size() << ", but x[0] has size " << x[0]->size() << ")");
        if (p.empty()) {
            std::vector<Real> val(x.size());
            for (Size k = 0; k < x[0]->size(); ++k) {
                if (!flt[k])
                    continue;
                for (Size c = 0; c < x.size(); ++c) {
                    val[c] = (*x[c])[k];
                }
                std::sort(val.begin(), val.end());
                for (Size c = 0; c < x.size(); ++c) {
                    y[c]->set(k, val[c]);
                }
            }
        } else {
            std::vector<std::pair<Real, Size>> val(x.size());
            for (Size k = 0; k < x[0]->size(); ++k) {
                if (!flt[k])
                    continue;
                for (Size c = 0; c < x.size(); ++c) {
                    val[c].first = (*x[c])[k];
                    val[c].second = c + 1; // permutation should start at 1
                }
                std::sort(val.begin(), val.end(), [](const std::pair<Real, Size>& x, const std::pair<Real, Size>& y) {
                    return x.first < y.first;
                });
                for (Size c = 0; c < x.size(); ++c) {
                    y[c]->set(k, val[c].first);
                    p[c]->set(k, static_cast<Real>(val[c].second));
                }
            }
        }

        TRACE("sort(...)", n); // TODO what would be a helpful output?
    }

    void visit(PermuteNode& n) override {
        checkpoint(n);

        std::vector<RandomVariable*> x, y, p;

        if (n.args[0]) {
            auto xname = boost::dynamic_pointer_cast<VariableNode>(n.args[0]);
            QL_REQUIRE(xname, "x must be a variable");
            QL_REQUIRE(!xname->args[0], "x must not be indexed");
            auto xv = context_.arrays.find(xname->name);
            QL_REQUIRE(xv != context_.arrays.end(), "did not find array with name '" << xname->name << "'");
            for (Size c = 0; c < xv->second.size(); ++c) {
                QL_REQUIRE(xv->second[c].which() == ValueTypeWhich::Number, "x must be NUMBER");
                x.push_back(&boost::get<RandomVariable>(xv->second[c]));
            }
        }

        if (n.args[1]) {
            auto yname = boost::dynamic_pointer_cast<VariableNode>(n.args[1]);
            QL_REQUIRE(yname, "y must be a variable");
            QL_REQUIRE(!yname->args[0], "y must not be indexed");
            auto yv = context_.arrays.find(yname->name);
            QL_REQUIRE(yv != context_.arrays.end(), "did not find array with name '" << yname->name << "'");
            for (Size c = 0; c < yv->second.size(); ++c) {
                QL_REQUIRE(yv->second[c].which() == ValueTypeWhich::Number, "y must be NUMBER");
                y.push_back(&boost::get<RandomVariable>(yv->second[c]));
            }
        }

        if (n.args[2]) {
            auto pname = boost::dynamic_pointer_cast<VariableNode>(n.args[2]);
            QL_REQUIRE(pname, "p must be a variable");
            QL_REQUIRE(!pname->args[0], "p must not be indexed");
            auto pv = context_.arrays.find(pname->name);
            QL_REQUIRE(pv != context_.arrays.end(), "did not find array with name '" << pname->name << "'");
            for (Size c = 0; c < pv->second.size(); ++c) {
                QL_REQUIRE(pv->second[c].which() == ValueTypeWhich::Number, "p must be NUMBER");
                p.push_back(&boost::get<RandomVariable>(pv->second[c]));
            }
        }

        // x should be source, y target and p permutation

        if (p.empty()) {
            p = y;
            y = x;
        }

        Size N = x.size();

        QL_REQUIRE(y.size() == N, "y array size (" << y.size() << ") must match x array size (" << N << ")");
        QL_REQUIRE(p.size() == N, "p array size (" << p.size() << ") must match x array size (" << N << ")");
        QL_REQUIRE(N >= 1, "array size must be >= 1");

        for (Size c = 0; c < N; ++c) {
            QL_REQUIRE(x[c]->size() == y[c]->size(), "x[" << c << "] size (" << x[c]->size() << ") must match y[" << c
                                                          << "] size (" << y[c]->size() << ")");
            QL_REQUIRE(x[c]->size() == p[c]->size(), "x[" << c << "] size (" << x[c]->size() << ") must match p[" << c
                                                          << "] size (" << p[c]->size() << ")");
        }

        const Filter& flt = filter.top();
        QL_REQUIRE(flt.size() == x[0]->size(),
                   "filter has size " << flt.size() << ", but x[0] has size " << x[0]->size() << ")");

        std::vector<Real> val(N);
        for (Size k = 0; k < x[0]->size(); ++k) {
            if (!flt[k])
                continue;
            for (Size c = 0; c < N; ++c) {
                Size permutedIndex = std::lround((*p[c])[k]);
                QL_REQUIRE(permutedIndex >= 1 && permutedIndex <= N, "permuted index p[" << c << "] = " << permutedIndex
                                                                                         << " out of bounds 1..." << N
                                                                                         << " at component " << k);
                val[c] = (*x[permutedIndex - 1])[k];
            }
            for (Size c = 0; c < N; ++c) {
                y[c]->set(k, val[c]);
            }
        }

        TRACE("permute(...)", n); // TODO what would be a helpful output?
    }

    // model dependent function nodes

    void visit(FunctionBlackNode& n) override {
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        n.args[2]->accept(*this);
        n.args[3]->accept(*this);
        n.args[4]->accept(*this);
        n.args[5]->accept(*this);
        checkpoint(n);

        auto impliedvol = value.pop();
        auto forward = value.pop();
        auto strike = value.pop();
        auto expirydate = value.pop();
        auto obsdate = value.pop();
        auto callput = value.pop();

        QL_REQUIRE(callput.which() == ValueTypeWhich::Number, "callput must be NUMBER");
        QL_REQUIRE(obsdate.which() == ValueTypeWhich::Event, "obsdate must be EVENT");
        QL_REQUIRE(expirydate.which() == ValueTypeWhich::Event, "expirydate must be EVENT");
        QL_REQUIRE(strike.which() == ValueTypeWhich::Number, "strike must be NUMBER");
        QL_REQUIRE(forward.which() == ValueTypeWhich::Number, "forward must be NUMBER");
        QL_REQUIRE(forward.which() == ValueTypeWhich::Number, "impliedvol must be NUMBER");

        RandomVariable omega = boost::get<RandomVariable>(callput);
        Date obs = boost::get<EventVec>(obsdate).value;
        Date expiry = boost::get<EventVec>(expirydate).value;
        RandomVariable k = boost::get<RandomVariable>(strike);
        RandomVariable f = boost::get<RandomVariable>(forward);
        RandomVariable v = boost::get<RandomVariable>(impliedvol);

        QL_REQUIRE(model_, "model is null");

        QL_REQUIRE(obs <= expiry, "obsdate (" << obs << ") must be <= expirydate (" << expiry << ")");
        RandomVariable t(model_->size(), model_->dt(obs, expiry));

        value.push(black(omega, t, k, f, v));
        TRACE("black( " << callput << " , " << obsdate << " , " << expirydate << " , " << strike << " , " << forward
                        << " , " << impliedvol << " ), t=" << t,
              n);
    }

    void payHelper(ASTNode& n, const bool log) {
        n.args[2]->accept(*this);
        auto paydate = value.pop();
        checkpoint(n);
        QL_REQUIRE(paydate.which() == ValueTypeWhich::Event, "paydate must be EVENT");
        QL_REQUIRE(model_, "model is null");
        // handle case of past payments: do not evaluate the other parameters, since not needed (e.g. past fixings)
        Date pay = boost::get<EventVec>(paydate).value;
        if (pay <= model_->referenceDate() && !log) {
            value.push(RandomVariable(size_, 0.0));
            TRACE("pay() = 0, since paydate " << paydate << " <= " << model_->referenceDate(), n);
        } else {
            n.args[0]->accept(*this);
            n.args[1]->accept(*this);
            n.args[3]->accept(*this);
            auto paycurr = value.pop();
            auto obsdate = value.pop();
            auto amount = value.pop();
            checkpoint(n);
            QL_REQUIRE(amount.which() == ValueTypeWhich::Number, "amount must be NUMBER");
            QL_REQUIRE(obsdate.which() == ValueTypeWhich::Event, "obsdate must be EVENT");
            QL_REQUIRE(paycurr.which() == ValueTypeWhich::Currency, "paycurr must be CURRENCY");
            Date obs = boost::get<EventVec>(obsdate).value;
            std::string pccy = boost::get<CurrencyVec>(paycurr).value;
            QL_REQUIRE(obs <= pay, "observation date (" << obs << ") <= payment date (" << pay << ") required");
            RandomVariable result = pay <= model_->referenceDate()
                                        ? RandomVariable(model_->size(), 0.0)
                                        : model_->pay(boost::get<RandomVariable>(amount), obs, pay, pccy);
            RandomVariable cashflowResult =
                pay <= model_->referenceDate() ? boost::get<RandomVariable>(amount) : result;
            if (!log || paylog_ == nullptr) {
                TRACE("pay( " << amount << " , " << obsdate << " , " << paydate << " , " << paycurr << " )", n);
            } else {
                // cashflow logging
                FunctionLogPayNode& pn = dynamic_cast<FunctionLogPayNode&>(n); // throws bad_cast if n has wrong type
                long legno = 0, slot = 0;
                std::string cftype = "Unspecified";
                if (pn.args[4]) {
                    pn.args[4]->accept(*this);
                    auto s = value.pop();
                    QL_REQUIRE(s.which() == ValueTypeWhich::Number, "legno must be NUMBER");
                    RandomVariable sv = boost::get<RandomVariable>(s);
                    sv.updateDeterministic();
                    QL_REQUIRE(sv.deterministic(), "legno must be deterministic");
                    legno = std::lround(sv.at(0));
                    QL_REQUIRE(slot >= 0, " legNo must be >= 0");
                    QL_REQUIRE(pn.args[5], "expected cashflow type argument when legno is given");
                    auto cftname = boost::dynamic_pointer_cast<VariableNode>(n.args[5]);
                    QL_REQUIRE(cftname, "cashflow type must be a variable name");
                    QL_REQUIRE(!cftname->args[0], "cashflow type must not be indexed");
                    cftype = cftname->name;
                    if (pn.args[6]) {
                        pn.args[6]->accept(*this);
                        auto s = value.pop();
                        QL_REQUIRE(s.which() == ValueTypeWhich::Number, "slot must be NUMBER");
                        RandomVariable sv = boost::get<RandomVariable>(s);
                        sv.updateDeterministic();
                        QL_REQUIRE(sv.deterministic(), "slot must be deterministic");
                        slot = std::lround(sv.at(0));
                        QL_REQUIRE(slot >= 1, " slot must be >= 1");
                    }
                }
                paylog_->write(cashflowResult, filter.top(), obs, pay, pccy, static_cast<Size>(legno), cftype,
                               static_cast<Size>(slot));
                TRACE("logpay( " << amount << " , " << obsdate << " , " << paydate << " , " << paycurr << " , " << legno
                                 << " , " << cftype << " , " << slot << ")",
                      n);
            }
            // push result
            value.push(result);
        }
    }

    void visit(FunctionPayNode& n) override { payHelper(n, false); }

    void visit(FunctionLogPayNode& n) override { payHelper(n, true); }

    void processNpvNode(ASTNode& n, bool hasMemSlot) {
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        ValueType memSlot;
        if (hasMemSlot) {
            n.args[2]->accept(*this);
            memSlot = value.pop();
        }
        auto obsdate = value.pop();
        auto amount = value.pop();
        checkpoint(n);
        Filter regFilter;
        Size opt = hasMemSlot ? 3 : 2;
        if (n.args[opt]) {
            n.args[opt]->accept(*this);
            auto val = value.pop();
            checkpoint(n);
            QL_REQUIRE(val.which() == ValueTypeWhich::Filter, "filter must be condition");
            regFilter = boost::get<Filter>(val);
        }
        RandomVariable addRegressor1, addRegressor2;
        if (n.args[opt + 1]) {
            n.args[opt + 1]->accept(*this);
            auto val = value.pop();
            checkpoint(n);
            QL_REQUIRE(val.which() == ValueTypeWhich::Number, "addRegressor1 must be NUMBER");
            addRegressor1 = boost::get<RandomVariable>(val);
        }
        if (n.args[opt + 2]) {
            n.args[opt + 2]->accept(*this);
            auto val = value.pop();
            checkpoint(n);
            QL_REQUIRE(val.which() == ValueTypeWhich::Number, "addRegressor2 must be NUMBER");
            addRegressor2 = boost::get<RandomVariable>(val);
        }
        QL_REQUIRE(amount.which() == ValueTypeWhich::Number, "amount must be NUMBER");
        QL_REQUIRE(obsdate.which() == ValueTypeWhich::Event, "obsdate must be EVENT");
        if (hasMemSlot) {
            QL_REQUIRE(memSlot.which() == ValueTypeWhich::Number, "memorySlot must be NUMBER");
        }
        QL_REQUIRE(model_, "model is null");
        Date obs = boost::get<EventVec>(obsdate).value;
        // roll back to past dates is treated as roll back to TODAY for convenience
        obs = std::max(obs, model_->referenceDate());
        boost::optional<long> mem(boost::none);
        if (hasMemSlot) {
            RandomVariable v = boost::get<RandomVariable>(memSlot);
            QL_REQUIRE(v.deterministic(), "memory slot must be deterministic");
            mem = static_cast<long>(v.at(0));
        }
        value.push(model_->npv(boost::get<RandomVariable>(amount), obs, regFilter, mem, addRegressor1, addRegressor2));
        if (hasMemSlot) {
            TRACE("npvmem( " << amount << " , " << obsdate << " , " << memSlot << " , " << regFilter << " , "
                             << addRegressor1 << " , " << addRegressor2 << " )",
                  n);
        } else {
            TRACE("npv( " << amount << " , " << obsdate << " , " << regFilter << " , " << addRegressor1 << " , "
                          << addRegressor2 << " )",
                  n);
        }
    }

    void visit(FunctionNpvNode& n) override { processNpvNode(n, false); }

    void visit(FunctionNpvMemNode& n) override { processNpvNode(n, true); }

    void visit(HistFixingNode& n) override {
        checkpoint(n);
        QL_REQUIRE(model_, "model is null");
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        auto obsdate = value.pop();
        auto underlying = value.pop();
        checkpoint(n);
        QL_REQUIRE(underlying.which() == ValueTypeWhich::Index, "underlying must be INDEX");
        QL_REQUIRE(obsdate.which() == ValueTypeWhich::Event, "obsdate must be EVENT");
        Date obs = boost::get<EventVec>(obsdate).value;
        std::string und = boost::get<IndexVec>(underlying).value;
        // if observation date is in the future, the answer is always zero
        if (obs > model_->referenceDate())
            value.push(RandomVariable(model_->size(), 0));
        else {
            // otherwise check whether a fixing is present in the historical time series
            TimeSeries<Real> series = IndexManager::instance().getHistory(IndexInfo(und).index()->name());
            if (series[obs] == Null<Real>())
                value.push(RandomVariable(model_->size(), 0.0));
            else
                value.push(RandomVariable(model_->size(), 1.0));
        }
        TRACE("histfixing( " << underlying << " , " << obsdate << " )", n);
    }

    void visit(FunctionDiscountNode& n) override {
        checkpoint(n);
        QL_REQUIRE(model_, "model is null");
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        n.args[2]->accept(*this);
        auto paycurr = value.pop();
        auto paydate = value.pop();
        auto obsdate = value.pop();
        checkpoint(n);
        QL_REQUIRE(obsdate.which() == ValueTypeWhich::Event, "obsdate must be EVENT");
        QL_REQUIRE(paydate.which() == ValueTypeWhich::Event, "paydate must be EVENT");
        QL_REQUIRE(paycurr.which() == ValueTypeWhich::Currency, "paycurr must be CURRENCY");
        Date obs = boost::get<EventVec>(obsdate).value;
        Date pay = boost::get<EventVec>(paydate).value;
        QL_REQUIRE(obs >= model_->referenceDate(),
                   "observation date (" << obs << ") >= reference date (" << model_->referenceDate() << ") required");
        QL_REQUIRE(obs <= pay, "observation date (" << obs << ") <= payment date (" << pay << ") required");
        value.push(model_->discount(obs, pay, boost::get<CurrencyVec>(paycurr).value));
        TRACE("discount( " << obsdate << " , " << paydate << " , " << paycurr << " )", n);
    }

    void processFwdCompAvgNode(ASTNode& n, const bool isAvg) {
        checkpoint(n);
        QL_REQUIRE(model_, "model is null");
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        n.args[2]->accept(*this);
        n.args[3]->accept(*this);
        auto enddate = value.pop();
        auto startdate = value.pop();
        auto obsdate = value.pop();
        auto underlying = value.pop();
        checkpoint(n);
        QL_REQUIRE(underlying.which() == ValueTypeWhich::Index, "underlying must be INDEX");
        QL_REQUIRE(obsdate.which() == ValueTypeWhich::Event, "obsdate must be EVENT");
        QL_REQUIRE(startdate.which() == ValueTypeWhich::Event, "start must be EVENT");
        QL_REQUIRE(enddate.which() == ValueTypeWhich::Event, "end must be EVENT");
        Date obs = boost::get<EventVec>(obsdate).value;
        Date start = boost::get<EventVec>(startdate).value;
        Date end = boost::get<EventVec>(enddate).value;
        QL_REQUIRE(obs <= start, "observation date (" << obs << ") must be <= start date (" << start << ")");
        QL_REQUIRE(start < end, "start date (" << start << ") must be < end date (" << end << ")");
        RandomVariable spreadValue(model_->size(), 0.0);
        RandomVariable gearingValue(model_->size(), 1.0);
        RandomVariable lookbackValue(model_->size(), 0.0);
        RandomVariable rateCutoffValue(model_->size(), 0.0);
        RandomVariable fixingDaysValue(model_->size(), 0.0);
        RandomVariable includeSpreadValue(model_->size(), -1.0);
        RandomVariable capValue(model_->size(), 999999.0);
        RandomVariable floorValue(model_->size(), -999999.0);
        RandomVariable nakedOptionValue(model_->size(), -1.0);
        RandomVariable localCapFloorValue(model_->size(), -1.0);
        if (n.args[4]) {
            QL_REQUIRE(n.args[5], "internal error: Fwd[Comp|Avg]: if spread is given, gearing must be given too");
            n.args[4]->accept(*this);
            auto spread = value.pop();
            QL_REQUIRE(spread.which() == ValueTypeWhich::Number, "spread must be NUMBER");
            spreadValue = boost::get<RandomVariable>(spread);
            QL_REQUIRE(spreadValue.deterministic(), "spread must be deterministic");
            n.args[5]->accept(*this);
            auto gearing = value.pop();
            QL_REQUIRE(gearing.which() == ValueTypeWhich::Number, "gearing must be NUMBER");
            gearingValue = boost::get<RandomVariable>(gearing);
            QL_REQUIRE(gearingValue.deterministic(), "gearing must be deterministic");
            checkpoint(n);
        }
        if (n.args[6]) {
            QL_REQUIRE(n.args[7] && n.args[8] && n.args[9],
                       "internal error: Fwd[Comp|Avg]: if lookback is given, rateCutoff, fixingDays and includeSpread "
                       "must be given too");
            n.args[6]->accept(*this);
            auto lookback = value.pop();
            QL_REQUIRE(lookback.which() == ValueTypeWhich::Number, "lookback must be NUMBER");
            lookbackValue = boost::get<RandomVariable>(lookback);
            QL_REQUIRE(lookbackValue.deterministic(), "lookback must be deterministic");
            n.args[7]->accept(*this);
            auto rateCutoff = value.pop();
            QL_REQUIRE(rateCutoff.which() == ValueTypeWhich::Number, "rateCutoff must be NUMBER");
            rateCutoffValue = boost::get<RandomVariable>(rateCutoff);
            QL_REQUIRE(rateCutoffValue.deterministic(), "rateCutoff must be deterministic");
            n.args[8]->accept(*this);
            auto fixingDays = value.pop();
            QL_REQUIRE(fixingDays.which() == ValueTypeWhich::Number, "fixingDays must be NUMBER");
            fixingDaysValue = boost::get<RandomVariable>(fixingDays);
            QL_REQUIRE(fixingDaysValue.deterministic(), "fixingDays must be deterministic");
            n.args[9]->accept(*this);
            auto includeSpread = value.pop();
            QL_REQUIRE(includeSpread.which() == ValueTypeWhich::Number, "lookback must be NUMBER");
            includeSpreadValue = boost::get<RandomVariable>(includeSpread);
            QL_REQUIRE(includeSpreadValue.deterministic() && (QuantLib::close_enough(includeSpreadValue.at(0), 1.0) ||
                                                              QuantLib::close_enough(includeSpreadValue.at(0), -1.0)),
                       "includeSpread must be deterministic and +1 or -1");
            checkpoint(n);
        }
        if (n.args[10]) {
            QL_REQUIRE(
                n.args[11] && n.args[12] && n.args[13],
                "internal error: Fwd[Comp|Avg]: if cap is given, floor, nakedOption, localCapFloor must be given too");
            n.args[10]->accept(*this);
            auto cap = value.pop();
            QL_REQUIRE(cap.which() == ValueTypeWhich::Number, "cap must be NUMBER");
            capValue = boost::get<RandomVariable>(cap);
            QL_REQUIRE(capValue.deterministic(), "cap must be deterministic");
            n.args[11]->accept(*this);
            auto floor = value.pop();
            QL_REQUIRE(floor.which() == ValueTypeWhich::Number, "floor must be NUMBER");
            floorValue = boost::get<RandomVariable>(floor);
            QL_REQUIRE(floorValue.deterministic(), "floor must be deterministic");
            n.args[12]->accept(*this);
            auto nakedOption = value.pop();
            QL_REQUIRE(nakedOption.which() == ValueTypeWhich::Number, "nakedOption must be NUMBER");
            nakedOptionValue = boost::get<RandomVariable>(nakedOption);
            QL_REQUIRE(nakedOptionValue.deterministic() && (QuantLib::close_enough(nakedOptionValue.at(0), 1.0) ||
                                                            QuantLib::close_enough(nakedOptionValue.at(0), -1.0)),
                       "nakedOption must be deterministic and +1 or -1");
            n.args[13]->accept(*this);
            auto localCapFloor = value.pop();
            QL_REQUIRE(localCapFloor.which() == ValueTypeWhich::Number, "localCapFloor must be NUMBER");
            localCapFloorValue = boost::get<RandomVariable>(localCapFloor);
            QL_REQUIRE(localCapFloorValue.deterministic() && (QuantLib::close_enough(localCapFloorValue.at(0), 1.0) ||
                                                              QuantLib::close_enough(localCapFloorValue.at(0), -1.0)),
                       "localCapFloor must be deterministic and +1 or -1");
            checkpoint(n);
        }

        bool includeSpreadBool = QuantLib::close_enough(includeSpreadValue.at(0), 1.0);
        bool nakedOptionBool = QuantLib::close_enough(nakedOptionValue.at(0), 1.0);
        bool localCapFloorBool = QuantLib::close_enough(localCapFloorValue.at(0), 1.0);

        value.push(model_->fwdCompAvg(isAvg, boost::get<IndexVec>(underlying).value, obs, start, end, spreadValue.at(0),
                                      gearingValue.at(0), static_cast<Integer>(lookbackValue.at(0)),
                                      static_cast<Natural>(rateCutoffValue.at(0)),
                                      static_cast<Natural>(fixingDaysValue.at(0)), includeSpreadBool, capValue.at(0),
                                      floorValue.at(0), nakedOptionBool, localCapFloorBool));

        TRACE("fwdCompAvg(" << isAvg << " , " << underlying << " , " << obsdate << " , " << startdate << " , "
                            << enddate << " , " << spreadValue.at(0) << " , " << gearingValue.at(0) << " , "
                            << lookbackValue.at(0) << " , " << rateCutoffValue.at(0) << " , " << fixingDaysValue.at(0)
                            << " , " << includeSpreadBool << " , " << capValue.at(0) << " , " << floorValue << " , "
                            << nakedOptionBool << " , " << localCapFloorBool << ")",
              n);
    }

    void visit(FunctionFwdCompNode& n) override { processFwdCompAvgNode(n, false); }

    void visit(FunctionFwdAvgNode& n) override { processFwdCompAvgNode(n, true); }

    void processProbNode(ASTNode& n, const bool above) {
        checkpoint(n);
        QL_REQUIRE(model_, "model is null");
        n.args[0]->accept(*this);
        n.args[1]->accept(*this);
        n.args[2]->accept(*this);
        n.args[3]->accept(*this);
        auto barrier = value.pop();
        auto obsdate2 = value.pop();
        auto obsdate1 = value.pop();
        auto underlying = value.pop();
        checkpoint(n);
        QL_REQUIRE(underlying.which() == ValueTypeWhich::Index, "underlying must be INDEX");
        QL_REQUIRE(obsdate1.which() == ValueTypeWhich::Event, "obsdate1 must be EVENT");
        QL_REQUIRE(obsdate2.which() == ValueTypeWhich::Event, "obsdate2 must be EVENT");
        QL_REQUIRE(barrier.which() == ValueTypeWhich::Number, "barrier must be NUMBER");
        std::string und = boost::get<IndexVec>(underlying).value;
        Date obs1 = boost::get<EventVec>(obsdate1).value;
        Date obs2 = boost::get<EventVec>(obsdate2).value;
        RandomVariable barrierValue = boost::get<RandomVariable>(barrier);
        if (obs1 > obs2)
            value.push(RandomVariable(model_->size(), 0.0));
        else
            value.push(model_->barrierProbability(und, obs1, obs2, barrierValue, above));
        TRACE((above ? "above" : "below")
                  << "prob(" << underlying << " , " << obsdate1 << " , " << obsdate2 << " , " << barrier << ")",
              n);
    }

    void visit(FunctionAboveProbNode& n) override { processProbNode(n, true); }

    void visit(FunctionBelowProbNode& n) override { processProbNode(n, false); }

    void visit(VarEvaluationNode& n) override {
        n.args[0]->accept(*this);
        checkpoint(n);
        n.args[1]->accept(*this);
        auto right = value.pop();
        auto left = value.pop();
        QL_REQUIRE(left.which() == ValueTypeWhich::Index,
                   "evaluation operator () can only be applied to an INDEX, got " << valueTypeLabels.at(left.which()));
        QL_REQUIRE(right.which() == ValueTypeWhich::Event,
                   "evaluation operator () argument obsDate must be EVENT, got " << valueTypeLabels.at(right.which()));
        checkpoint(n);
        Date obs = boost::get<EventVec>(right).value, fwd = Null<Date>();
        QL_REQUIRE(model_, "model is null");
        if (n.args[2]) {
            n.args[2]->accept(*this);
            auto fwdDate = value.pop();
            checkpoint(n);
            QL_REQUIRE(fwdDate.which() == ValueTypeWhich::Event,
                       "evaluation operator () argument fwdDate must be EVENT, got "
                           << valueTypeLabels.at(fwdDate.which()));
            fwd = boost::get<EventVec>(fwdDate).value;
            if (fwd == obs)
                fwd = Null<Date>();
            else {
                QL_REQUIRE(obs < fwd,
                           "evaluation operator() requires obsDate (" << obs << ") < fwdDate (" << fwd << ")");
            }
        }
        value.push(model_->eval(boost::get<IndexVec>(left).value, obs, fwd));
        TRACE("indexEval( " << left << " , " << right << " , " << fwd << " )", n);
    }

    // inputs
    const boost::shared_ptr<Model> model_;
    const Size size_;
    const std::string script_;
    bool& interactive_;
    boost::shared_ptr<PayLog> paylog_; // cashflow log
    // working variables
    Context& context_;
    ASTNode*& lastVisitedNode_;
    // state of the runner
    SafeStack<Filter> filter;
    SafeStack<ValueType> value;
};

} // namespace

void ScriptEngine::run(const std::string& script, bool interactive, boost::shared_ptr<PayLog> paylog) {

    ASTNode* loc;
    ASTRunner runner(model_, script, interactive, *context_, loc, paylog);

    randomvariable_output_pattern pattern;
    if (model_ == nullptr || model_->type() == Model::Type::MC) {
        pattern = randomvariable_output_pattern(randomvariable_output_pattern::pattern::expectation);
    } else if (model_->type() == Model::Type::FD) {
        pattern = randomvariable_output_pattern(randomvariable_output_pattern::pattern::left_middle_right);
    } else {
        QL_FAIL("model type not handled when setting output pattern for random variables");
    }

    DLOG("run script engine, context before run is:");
    DLOGGERSTREAM(pattern << *context_);

    if (interactive) {
        std::cerr << pattern << "\nInitial Context: \n" << (*context_) << std::endl;
    }

    boost::timer::cpu_timer timer;
    try {
        reset(root_);
        root_->accept(runner);
        timer.stop();
        QL_REQUIRE(runner.value.size() == 1,
                   "ScriptEngine::run(): value stack has wrong size (" << runner.value.size() << "), should be 1");
        QL_REQUIRE(runner.filter.size() == 1,
                   "ScriptEngine::run(): filter stack has wrong size (" << runner.filter.size() << "), should be 1");
        DLOG("script engine successfully finished, context after run is:");

        if (interactive) {
            std::cerr << "\nScript engine finished without errors. Context after run:" << std::endl;
        }

    } catch (const std::exception& e) {
        std::ostringstream errorMessage;
        errorMessage << "Error during script execution: " << e.what() << " at "
                     << (loc ? to_string(loc->locationInfo) : "(last visited ast node not known)") << ": "
                     << printCodeContext(script, loc, true);

        std::ostringstream strippedErrorMsg;
        strippedErrorMsg << "Error during script execution: " << e.what() << " at "
                         << (loc ? to_string(loc->locationInfo) : "(last visited ast node not known)");

        DLOGGERSTREAM(strippedErrorMsg.str());
        DLOGGERSTREAM(printCodeContext(script, loc));
        DLOGGERSTREAM("Context when hitting the error:");
        DLOGGERSTREAM(pattern << *context_);

        if (interactive) {
            std::cerr << strippedErrorMsg.str() << "\n";
            std::cerr << printCodeContext(script, loc);
            std::cerr << "Context when hitting the error:" << std::endl;
            std::cerr << (*context_) << std::endl;
            std::cin.get();
        }

        QL_FAIL(errorMessage.str());
    }

    DLOGGERSTREAM(pattern << *context_);
    DLOG("Script engine running time: " << boost::timer::format(timer.elapsed()));

    if (interactive) {
        std::cerr << pattern << *context_ << std::endl;
        std::cin.get();
    }
}

} // namespace data
} // namespace ore
