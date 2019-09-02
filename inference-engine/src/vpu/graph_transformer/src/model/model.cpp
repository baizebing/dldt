// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <vpu/model/model.hpp>

#include <cctype>
#include <memory>
#include <string>
#include <set>
#include <exception>
#include <algorithm>

#include <details/caseless.hpp>

#include <vpu/utils/auto_scope.hpp>
#include <vpu/utils/profiling.hpp>

namespace vpu {

//
// Resources
//

void printTo(std::ostream& os, const Resources& res) {
    os << "[" << std::endl;

    os << "numCMXSlices=" << res.numCMXSlices << std::endl;
    os << "numSHAVEs=" << res.numSHAVEs << std::endl;
    os << "cmxLimit=" << res.cmxLimit << std::endl;

    os << "]";
}

void printTo(DotLabel& lbl, const Resources& res) {
    DotLabel subLbl(lbl);
    subLbl.appendPair("numCMXSlices", res.numCMXSlices);
    subLbl.appendPair("numSHAVEs", res.numSHAVEs);
    subLbl.appendPair("cmxLimit", res.cmxLimit);
}

//
// Model
//

void Model::setBatchSize(int batchSize) {
    // Check `batchSize` value.
    VPU_THROW_UNLESS(batchSize >= 1);

    _batchSize = batchSize;
    _allocator.setBatchSize(batchSize);
}

Data Model::addInputData(
        const std::string& name,
        const DataDesc& desc) {
    std::shared_ptr<DataNode> data(new DataNode);

    data->_name = name;
    data->_usage = DataUsage::Input;
    data->_desc = desc;
    data->_model = handle_from_this();

    data->_ptrPosInModel = _dataPtrList.emplace(_dataPtrList.end(), data);
    _dataList.push_back(data);

    _allocator.setNeedToAllocNonIntermData();

    return data;
}

Data Model::addOutputData(
        const std::string& name,
        const DataDesc& desc) {
    std::shared_ptr<DataNode> data(new DataNode);

    data->_name = name;
    data->_usage = DataUsage::Output;
    data->_desc = desc;
    data->_model = handle_from_this();

    data->_ptrPosInModel = _dataPtrList.emplace(_dataPtrList.end(), data);
    _dataList.push_back(data);

    _allocator.setNeedToAllocNonIntermData();

    return data;
}

Data Model::addConstData(
        const std::string& name,
        const DataDesc& desc,
        const DataContent::Ptr& content) {
    IE_ASSERT(content != nullptr);

    std::shared_ptr<DataNode> data(new DataNode);

    data->_name = name;
    data->_usage = DataUsage::Const;
    data->_desc = desc;
    data->_model = handle_from_this();

    data->_content = content;
    content->_desc = desc;

    data->_ptrPosInModel = _dataPtrList.emplace(_dataPtrList.end(), data);
    _dataList.push_back(data);

    _allocator.setNeedToAllocNonIntermData();

    return data;
}

Data Model::addNewData(
        const std::string& name,
        const DataDesc& desc) {
    std::shared_ptr<DataNode> data(new DataNode);

    data->_name = name;
    data->_usage = DataUsage::Intermediate;
    data->_desc = desc;
    data->_model = handle_from_this();

    data->_ptrPosInModel = _dataPtrList.emplace(_dataPtrList.end(), data);
    _dataList.push_back(data);

    return data;
}

Data Model::addFakeData() {
    std::shared_ptr<DataNode> data(new DataNode);

    data->_name = "<fake>";
    data->_usage = DataUsage::Fake;
    data->_desc = DataDesc({1});
    data->_model = handle_from_this();

    data->_ptrPosInModel = _dataPtrList.emplace(_dataPtrList.end(), data);
    _dataList.push_back(data);

    return data;
}

Data Model::duplicateData(
        const Data& origData,
        const std::string& postfix,
        const DataDesc& newDesc,
        const DataContent::Ptr& newContent) {
    //
    // Check that the objects belong to the same Model.
    //

    IE_ASSERT(origData->_model.get() == this);

    //
    // Duplicate Data node.
    //

    auto newDataUsage = origData->usage();
    if (newDataUsage == DataUsage::Input ||
        newDataUsage == DataUsage::Output) {
        // Duplicates for Input & Output can be only Intermediate
        newDataUsage = DataUsage::Intermediate;
    }

    std::shared_ptr<DataNode> newData(new DataNode);

    newData->_name = origData->name() + postfix;
    newData->_usage = newDataUsage;
    newData->_desc = newDesc.numDims() != 0 ? newDesc : origData->desc();
    newData->_model = handle_from_this();

    if (newDataUsage == DataUsage::Const) {
        newData->_content = newContent != nullptr ? newContent : origData->content();
        if (newContent != nullptr) {
            newContent->_desc = newData->_desc;
        }
    }

    newData->attrs().copyFrom(origData->attrs());

    newData->_ptrPosInModel = _dataPtrList.emplace(_dataPtrList.end(), newData);
    _dataList.push_back(newData);

    return newData;
}

Stage Model::duplicateStage(
        const std::string& name,
        const Stage& origStage,
        const DataVector& inputs,
        const DataVector& outputs) {
    //
    // Check that the new Stage has inputs and outputs.
    //

    IE_ASSERT(!inputs.empty());
    IE_ASSERT(!outputs.empty());

    //
    // Check that the objects belong to the same Model.
    //

    IE_ASSERT(origStage->_model.get() == this);

    for (const auto& input : inputs) {
        IE_ASSERT(input->_model.get() == this);
    }

    for (const auto& output : outputs) {
        IE_ASSERT(output->_model.get() == this);
    }

    //
    // Check that there are no loops.
    //

    // TODO: more advanced check.
    for (const auto& output : outputs) {
        for (const auto& input : inputs) {
            IE_ASSERT(input != output);
        }
    }

    //
    // Create new Stage.
    //

    _resetStageOrder = true;;

    auto stage = origStage->cloneImpl();

    stage->_name = name;
    stage->_type = origStage->_type;
    stage->_origLayer = origStage->_origLayer;
    stage->_model = handle_from_this();

    _initialStages.emplace(stage);

    for (const auto& input : inputs) {
        addStageInput(stage, input);
    }
    for (const auto& output : outputs) {
        addStageOutput(stage, output);
    }
    for (const auto& tempBufferEdge : origStage->_tempBufferEdges) {
        addTempBuffer(stage, tempBufferEdge->tempBuffer()->desc());
    }

    stage->_ptrPosInModel = _stagePtrList.emplace(_stagePtrList.end(), stage);

    return stage;
}

StageInput Model::addStageInput(
        const Stage& stage,
        const Data& data) {
    //
    // Check that the objects belong to the same Model.
    //

    IE_ASSERT(stage->_model.get() == this);
    IE_ASSERT(data->_model.get() == this);

    // TODO: check for loops in the graph.

    //
    // Input data can't be Temp.
    //

    IE_ASSERT(data->_usage != DataUsage::Temp);

    //
    // Create new Edge.
    //

    _resetStageOrder = true;;

    std::shared_ptr<StageInputEdge> edge(new StageInputEdge);

    edge->_consumer = stage;
    edge->_input = data;
    edge->_portInd = stage->_inputEdges.size();
    edge->_model = handle_from_this();

    edge->_ptrPosInModel = _inEdgePtrList.emplace(_inEdgePtrList.end(), edge);
    data->_consumerEdges.push_back(edge);
    stage->_inputEdges.emplace_back(edge);

    //
    // Stage order helpers
    //

    if (data->_producerEdge != nullptr) {
        IE_ASSERT(stage->_parentStageEdge == nullptr);
        IE_ASSERT(data->_producerEdge->_producer->_parentStageEdge == nullptr);
        ++data->_producerEdge->_producer->_nextStages[stage];
        ++stage->_prevStages[data->_producerEdge->_producer];
    }

    if (stage->_prevStages.empty()) {
        _initialStages.emplace(stage);
    } else {
        _initialStages.erase(stage);
    }

    return edge;
}

StageOutput Model::addStageOutput(
        const Stage& stage,
        const Data& data) {
    //
    // Check that the objects belong to the same Model.
    //

    IE_ASSERT(stage->_model.get() == this);
    IE_ASSERT(data->_model.get() == this);

    //
    // Check that the `data` is free.
    //

    IE_ASSERT(data->_producerEdge == nullptr);

    if (data->_parentDataEdge != nullptr) {
        IE_ASSERT(data->_parentDataEdge->_order != SharedDataOrder::ParentWritesToChild);
    }

    for (const auto& childDataEdge : data->_childDataEdges) {
        IE_ASSERT(childDataEdge->_order != SharedDataOrder::ChildWritesToParent);
    }

    //
    // Output data can be Output, Intermediate, or Fake only.
    //

    IE_ASSERT(data->_usage == DataUsage::Output || data->_usage == DataUsage::Intermediate || data->_usage == DataUsage::Fake);

    // TODO: check for loops in the graph.

    _resetStageOrder = true;;

    std::shared_ptr<StageOutputEdge> edge(new StageOutputEdge);

    edge->_producer = stage;
    edge->_output = data;
    edge->_portInd = stage->_outputEdges.size();
    edge->_model = handle_from_this();

    edge->_ptrPosInModel = _outEdgePtrList.emplace(_outEdgePtrList.end(), edge);
    stage->_outputEdges.emplace_back(edge);
    data->_producerEdge = edge;

    //
    // Stage order helpers
    //

    for (const auto& consumerEdge : data->_consumerEdges) {
        IE_ASSERT(stage->_parentStageEdge == nullptr);
        IE_ASSERT(consumerEdge->_consumer->_parentStageEdge == nullptr);
        ++consumerEdge->_consumer->_prevStages[stage];
        ++stage->_nextStages[consumerEdge->_consumer];

        _initialStages.erase(consumerEdge->_consumer);
    }

    return edge;
}

StageTempBuffer Model::addTempBuffer(
        const Stage& stage,
        const DataDesc& desc) {
    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(stage->_model.get() == this);

    //
    // Create new Data.
    //

    std::shared_ptr<DataNode> data(new DataNode);

    data->_name = formatString("%s@temp@%d", stage->name(), stage->_tempBufferEdges.size() + 1);
    data->_usage = DataUsage::Temp;
    data->_desc = desc;
    data->_model = handle_from_this();

    data->_ptrPosInModel = _dataPtrList.emplace(_dataPtrList.end(), data);
    _dataList.push_back(data);

    //
    // Create new Edge.
    //

    std::shared_ptr<StageTempBufferEdge> edge(new StageTempBufferEdge);

    edge->_stage = stage;
    edge->_tempBuffer = data;
    edge->_portInd = stage->_tempBufferEdges.size();
    edge->_model = handle_from_this();

    edge->_ptrPosInModel = _tempBufferEdgePtrList.emplace(_tempBufferEdgePtrList.end(), edge);
    stage->_tempBufferEdges.emplace_back(edge);
    data->_tempBufferEdge = edge;

    return edge;
}

void Model::replaceStageInput(
        const StageInput& edge,
        const Data& newInput) {
    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(edge->_model.get() == this);
    IE_ASSERT(newInput->_model.get() == this);

    //
    // Check that there are no loops.
    //

    // TODO: more advanced check.
    for (const auto& output : edge->consumer()->outputs()) {
        IE_ASSERT(newInput != output);
    }

    //
    // Input data can't be Temp.
    //

    IE_ASSERT(newInput->_usage != DataUsage::Temp);

    //
    // Can't replace Edge from injected Stage.
    //

    IE_ASSERT(edge->_parentEdge == nullptr);
    IE_ASSERT(edge->_childEdge == nullptr);

    //
    // Edge change affects the Stage order.
    //

    _resetStageOrder = true;;

    //
    // Remove Edge from previous input.
    //

    edge->_input->_consumerEdges.erase(edge);

    //
    // Previous stage order helpers
    //

    if (edge->_input->_producerEdge != nullptr) {
        auto it1 = edge->_input->_producerEdge->_producer->_nextStages.find(edge->_consumer);
        IE_ASSERT(it1 != edge->_input->_producerEdge->_producer->_nextStages.end());
        --it1->second;
        if (it1->second <= 0) {
            edge->_input->_producerEdge->_producer->_nextStages.erase(it1);
        }

        auto it2 = edge->_consumer->_prevStages.find(edge->_input->_producerEdge->_producer);
        IE_ASSERT(it2 != edge->_consumer->_prevStages.end());
        --it2->second;
        if (it2->second <= 0) {
            edge->_consumer->_prevStages.erase(it2);
        }
    }

    //
    // Set new input.
    //

    edge->_input = newInput;
    newInput->_consumerEdges.push_back(edge);

    //
    // Stage order helpers
    //

    if (newInput->_producerEdge != nullptr) {
        IE_ASSERT(edge->_consumer->_parentStageEdge == nullptr);
        IE_ASSERT(newInput->_producerEdge->_producer->_parentStageEdge == nullptr);
        ++newInput->_producerEdge->_producer->_nextStages[edge->_consumer];
        ++edge->_consumer->_prevStages[newInput->_producerEdge->_producer];

        _initialStages.erase(edge->_consumer);
    }

    if (edge->_consumer->_prevStages.empty()) {
        _initialStages.emplace(edge->_consumer);
    } else {
        _initialStages.erase(edge->_consumer);
    }
}

void Model::replaceStageOutput(
        const StageOutput& edge,
        const Data& newOutput) {
    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(edge->_model.get() == this);
    IE_ASSERT(newOutput->_model.get() == this);

    //
    // Check that there are no loops.
    //

    // TODO: more advanced check.
    for (const auto& input : edge->producer()->inputs()) {
        IE_ASSERT(newOutput != input);
    }

    //
    // Check that `data` is free.
    //

    IE_ASSERT(newOutput->_producerEdge == nullptr);

    if (newOutput->_parentDataEdge != nullptr) {
        IE_ASSERT(newOutput->_parentDataEdge->_order != SharedDataOrder::ParentWritesToChild);
    }

    for (const auto& childDataEdge : newOutput->_childDataEdges) {
        IE_ASSERT(childDataEdge->_order != SharedDataOrder::ChildWritesToParent);
    }

    //
    // Output data can be Output/Intermediate/Fake.
    //

    IE_ASSERT(newOutput->_usage == DataUsage::Output ||
              newOutput->_usage == DataUsage::Intermediate ||
              newOutput->_usage == DataUsage::Fake);

    //
    // Can't replace Edge from injected Stage.
    //

    IE_ASSERT(edge->_parentEdge == nullptr);
    IE_ASSERT(edge->_childEdge == nullptr);

    //
    // Edge change affects the Stage order.
    //

    _resetStageOrder = true;;

    //
    // Remove Edge from previous output.
    //

    edge->_output->_producerEdge = nullptr;

    //
    // Previous stage order helpers
    //

    for (const auto& consumerEdge : edge->_output->_consumerEdges) {
        auto it1 = consumerEdge->_consumer->_prevStages.find(edge->_producer);
        IE_ASSERT(it1 != consumerEdge->_consumer->_prevStages.end());
        --it1->second;
        if (it1->second <= 0) {
            consumerEdge->_consumer->_prevStages.erase(it1);
        }

        auto it2 = edge->_producer->_nextStages.find(consumerEdge->_consumer);
        IE_ASSERT(it2 != edge->_producer->_nextStages.end());
        --it2->second;
        if (it2->second <= 0) {
            edge->_producer->_nextStages.erase(it2);
        }

        if (consumerEdge->_consumer->_prevStages.empty()) {
            _initialStages.emplace(consumerEdge->_consumer);
        } else {
            _initialStages.erase(consumerEdge->_consumer);
        }
    }

    //
    // Set new output.
    //

    edge->_output = newOutput;
    newOutput->_producerEdge = edge;

    //
    // Stage order helpers
    //

    for (const auto& consumerEdge : newOutput->_consumerEdges) {
        IE_ASSERT(edge->_producer->_parentStageEdge == nullptr);
        IE_ASSERT(consumerEdge->_consumer->_parentStageEdge == nullptr);
        ++consumerEdge->_consumer->_prevStages[edge->_producer];
        ++edge->_producer->_nextStages[consumerEdge->_consumer];

        _initialStages.erase(consumerEdge->_consumer);
    }
}

Model::InjectStageHelper::~InjectStageHelper() {
    //
    // Check that `done` was called.
    //

    if (_model != nullptr) {
        std::terminate();
    }
}

Model::InjectStageHelper& Model::InjectStageHelper::parentHW(const Stage& parent) {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that `parentHW` was not called.
    //

    IE_ASSERT(_parent == nullptr);

    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(parent->_model == _model);

    //
    // Check that `parent` is HW.
    //

    IE_ASSERT(parent->category() == StageCategory::HW);

    _parent = parent;

    return *this;
}

Model::InjectStageHelper& Model::InjectStageHelper::childSW(const Stage& child) {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that `childSW` was not called.
    //

    IE_ASSERT(_child == nullptr);

    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(child->_model == _model);

    //
    // Check that `parent` is HW.
    //

    IE_ASSERT(child->category() == StageCategory::DMA || child->category() == StageCategory::SHAVE);

    _child = child;

    return *this;
}

InjectedStage Model::InjectStageHelper::done() {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that all fields were set.
    //

    IE_ASSERT(_parent != nullptr);
    IE_ASSERT(_child != nullptr);

    //
    // Call actual implementation.
    //

    auto edge = _model->injectStageImpl(_parent, _child);

    //
    // Reset the internal state.
    //

    _model = nullptr;

    return edge;
}

InjectedStage Model::injectStageImpl(
        const Stage& parent,
        const Stage& child) {
    //
    // Check the parent and child was not already injected.
    //

    IE_ASSERT(parent->_parentStageEdge == nullptr);

    IE_ASSERT(child->_parentStageEdge == nullptr);
    IE_ASSERT(child->_injectedStageEdges.empty());

    //
    // New Edge affects the Stage order.
    //

    _resetStageOrder = true;;

    //
    // Create new Edge.
    //

    std::shared_ptr<InjectedStageEdge> edge(new InjectedStageEdge);

    edge->_parent = parent;
    edge->_child = child.lock();
    edge->_portInd = parent->_injectedStageEdges.size();
    edge->_model = handle_from_this();

    edge->_ptrPosInModel = _stageEdgePtrList.emplace(_stageEdgePtrList.end(), edge);
    parent->_injectedStageEdges.push_back(edge);

    child->_parentStageEdge = edge;

    //
    // Redirect child inputs to parent.
    //

    for (const auto& childInEdge : child->_inputEdges) {
        if (childInEdge->_input->_producerEdge != nullptr) {
            auto it1 = childInEdge->_input->_producerEdge->_producer->_nextStages.find(childInEdge->_consumer);
            IE_ASSERT(it1 != childInEdge->_input->_producerEdge->_producer->_nextStages.end());
            --it1->second;
            if (it1->second <= 0) {
                childInEdge->_input->_producerEdge->_producer->_nextStages.erase(it1);
            }

            auto it2 = childInEdge->_consumer->_prevStages.find(childInEdge->_input->_producerEdge->_producer);
            IE_ASSERT(it2 != childInEdge->_consumer->_prevStages.end());
            --it2->second;
            if (it2->second <= 0) {
                childInEdge->_consumer->_prevStages.erase(it2);
            }
        }

        childInEdge->_input->_consumerEdges.erase(childInEdge);

        auto parentInEdge = addStageInput(parent, childInEdge->_input);

        childInEdge->_parentEdge = parentInEdge;
        parentInEdge->_childEdge = childInEdge;
    }

    //
    // Redirect child outputs to parent.
    //

    for (const auto& childOutEdge : child->_outputEdges) {
        for (const auto& consumerEdge : childOutEdge->_output->_consumerEdges) {
            auto it1 = consumerEdge->_consumer->_prevStages.find(childOutEdge->_producer);
            IE_ASSERT(it1 != consumerEdge->_consumer->_prevStages.end());
            --it1->second;
            if (it1->second <= 0) {
                consumerEdge->_consumer->_prevStages.erase(it1);
            }

            auto it2 = childOutEdge->_producer->_nextStages.find(consumerEdge->_consumer);
            IE_ASSERT(it2 != childOutEdge->_producer->_nextStages.end());
            --it2->second;
            if (it2->second <= 0) {
                childOutEdge->_producer->_nextStages.erase(it2);
            }
        }

        childOutEdge->_output->_producerEdge = nullptr;

        auto parentOutEdge = addStageOutput(parent, childOutEdge->_output);

        childOutEdge->_parentEdge = parentOutEdge;
        parentOutEdge->_childEdge = childOutEdge;
    }

    //
    // Redirect child temp buffers to parent.
    //

    for (const auto& childEdge : child->_tempBufferEdges) {
        childEdge->_tempBuffer->_tempBufferEdge = nullptr;

        std::shared_ptr<StageTempBufferEdge> parentEdge(new StageTempBufferEdge);

        parentEdge->_stage = parent;
        parentEdge->_tempBuffer = childEdge->_tempBuffer;
        parentEdge->_portInd = parent->_tempBufferEdges.size();
        parentEdge->_model = handle_from_this();

        parentEdge->_ptrPosInModel = _tempBufferEdgePtrList.emplace(_tempBufferEdgePtrList.end(), parentEdge);

        parent->_tempBufferEdges.emplace_back(parentEdge);
        childEdge->_tempBuffer->_tempBufferEdge = parentEdge;

        childEdge->_parentEdge = parentEdge;
        parentEdge->_childEdge = childEdge;
    }

    //
    // Move child Stage from the Model to parent Stage.
    //

    IE_ASSERT(child->_ptrPosInModel != _stagePtrList.end());
    _stagePtrList.erase(child->_ptrPosInModel);
    child->_ptrPosInModel = _stagePtrList.end();

    _initialStages.erase(child);

    if (parent->_prevStages.empty()) {
        _initialStages.emplace(parent);
    } else {
        _initialStages.erase(parent);
    }

    return edge;
}

void Model::revertInjection(const InjectedStage& edge) {
    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(edge->_model.get() == this);

    auto parentStage = edge->_parent;
    auto childStage = edge->_child;

    IE_ASSERT(parentStage->_model.get() == this);
    IE_ASSERT(childStage->_model.get() == this);
    IE_ASSERT(childStage->_parentStageEdge == edge);

    //
    // The revert affects the Stage order.
    //

    _resetStageOrder = true;;

    //
    // Move child Stage from parent Stage to the Model.
    //

    childStage->_ptrPosInModel = _stagePtrList.emplace(_stagePtrList.end(), childStage);

    //
    // Remove InjectedStage Edge from parent and child Stage.
    //

    parentStage->_injectedStageEdges.erase(edge);
    childStage->_parentStageEdge = nullptr;

    //
    // Remove Injected Input Edges from parent Stage.
    //

    int startInd = -1;
    int endInd = -1;

    for (const auto& inEdge : parentStage->_inputEdges) {
        if (inEdge->_childEdge == nullptr) {
            IE_ASSERT(startInd < 0);
            continue;
        }

        if (startInd >= 0 && endInd >= 0) {
            IE_ASSERT(inEdge->_childEdge->_consumer != childStage);
        }

        if (inEdge->_childEdge->_consumer != childStage) {
            if (startInd >= 0 && endInd < 0) {
                endInd = inEdge->_portInd;
            }
            continue;
        }

        if (startInd < 0) {
            startInd = inEdge->_portInd;
        }
        if (inEdge->_portInd == parentStage->_inputEdges.size() - 1) {
            endInd = inEdge->_portInd + 1;
        }

        if (inEdge->_input->_producerEdge != nullptr) {
            auto it1 = inEdge->_input->_producerEdge->_producer->_nextStages.find(inEdge->_consumer);
            IE_ASSERT(it1 != inEdge->_input->_producerEdge->_producer->_nextStages.end());
            --it1->second;
            if (it1->second <= 0) {
                inEdge->_input->_producerEdge->_producer->_nextStages.erase(it1);
            }

            auto it2 = inEdge->_consumer->_prevStages.find(inEdge->_input->_producerEdge->_producer);
            IE_ASSERT(it2 != inEdge->_consumer->_prevStages.end());
            --it2->second;
            if (it2->second <= 0) {
                inEdge->_consumer->_prevStages.erase(it2);
            }
        }

        if (inEdge->_childEdge->_input->_producerEdge != nullptr) {
            IE_ASSERT(inEdge->_childEdge->_consumer->_parentStageEdge == nullptr);
            IE_ASSERT(inEdge->_childEdge->_input->_producerEdge->_producer->_parentStageEdge == nullptr);
            ++inEdge->_childEdge->_input->_producerEdge->_producer->_nextStages[inEdge->_childEdge->_consumer];
            ++inEdge->_childEdge->_consumer->_prevStages[inEdge->_childEdge->_input->_producerEdge->_producer];
        }

        inEdge->_childEdge->_parentEdge = nullptr;
        inEdge->_input->_consumerEdges.erase(inEdge);
        inEdge->_input->_consumerEdges.push_back(inEdge->_childEdge);

        IE_ASSERT(inEdge->_ptrPosInModel != _inEdgePtrList.end());
        _inEdgePtrList.erase(inEdge->_ptrPosInModel);
    }

    IE_ASSERT(startInd >= 0 && endInd > startInd && startInd <= parentStage->_inputEdges.size());
    parentStage->_inputEdges.erase(
        parentStage->_inputEdges.begin() + startInd,
        parentStage->_inputEdges.begin() + endInd);

    for (int i = 0; i < parentStage->_inputEdges.size(); ++i) {
        parentStage->_inputEdges[i]->_portInd = i;
    }

    //
    // Remove Injected Output Edges from parent Stage.
    //

    startInd = -1;
    endInd = -1;

    for (const auto& outEdge : parentStage->_outputEdges) {
        if (outEdge->_childEdge == nullptr) {
            IE_ASSERT(startInd < 0);
            continue;
        }

        if (startInd >= 0 && endInd >= 0) {
            IE_ASSERT(outEdge->_childEdge->_producer != childStage);
        }

        if (outEdge->_childEdge->_producer != childStage) {
            if (startInd >= 0 && endInd < 0) {
                endInd = outEdge->_portInd;
            }
            continue;
        }

        if (startInd < 0) {
            startInd = outEdge->_portInd;
        }
        if (outEdge->_portInd == parentStage->_outputEdges.size() - 1) {
            endInd = outEdge->_portInd + 1;
        }

        for (const auto& consumerEdge : outEdge->_output->_consumerEdges) {
            auto it1 = consumerEdge->_consumer->_prevStages.find(outEdge->_producer);
            IE_ASSERT(it1 != consumerEdge->_consumer->_prevStages.end());
            --it1->second;
            if (it1->second <= 0) {
                consumerEdge->_consumer->_prevStages.erase(it1);
            }

            auto it2 = outEdge->_producer->_nextStages.find(consumerEdge->_consumer);
            IE_ASSERT(it2 != outEdge->_producer->_nextStages.end());
            --it2->second;
            if (it2->second <= 0) {
                outEdge->_producer->_nextStages.erase(it2);
            }
        }

        for (const auto& consumerEdge : outEdge->_childEdge->_output->_consumerEdges) {
            IE_ASSERT(outEdge->_childEdge->_producer->_parentStageEdge == nullptr);
            IE_ASSERT(consumerEdge->_consumer->_parentStageEdge == nullptr);
            ++consumerEdge->_consumer->_prevStages[outEdge->_childEdge->_producer];
            ++outEdge->_childEdge->_producer->_nextStages[consumerEdge->_consumer];
        }

        outEdge->_childEdge->_parentEdge = nullptr;
        outEdge->_output->_producerEdge = outEdge->_childEdge;

        IE_ASSERT(outEdge->_ptrPosInModel != _outEdgePtrList.end());
        _outEdgePtrList.erase(outEdge->_ptrPosInModel);
    }

    IE_ASSERT(startInd >= 0 && endInd > startInd && startInd <= parentStage->_outputEdges.size());
    parentStage->_outputEdges.erase(
        parentStage->_outputEdges.begin() + startInd,
        parentStage->_outputEdges.begin() + endInd);

    for (int i = 0; i < parentStage->_outputEdges.size(); ++i) {
        parentStage->_outputEdges[i]->_portInd = i;
    }

    //
    // Remove Injected Temp Buffer Edges from parent Stage.
    //

    startInd = -1;
    endInd = -1;

    for (const auto& tempBufferEdge : parentStage->_tempBufferEdges) {
        if (tempBufferEdge->_childEdge == nullptr) {
            IE_ASSERT(startInd < 0);
            continue;
        }

        if (startInd >= 0 && endInd >= 0) {
            IE_ASSERT(tempBufferEdge->_childEdge->_stage != childStage);
        }

        if (tempBufferEdge->_childEdge->_stage != childStage) {
            if (startInd >= 0 && endInd < 0) {
                endInd = tempBufferEdge->_portInd;
            }
            continue;
        }

        if (startInd < 0) {
            startInd = tempBufferEdge->_portInd;
        }
        if (tempBufferEdge->_portInd == parentStage->_tempBufferEdges.size() - 1) {
            endInd = tempBufferEdge->_portInd + 1;
        }

        tempBufferEdge->_childEdge->_parentEdge = nullptr;
        tempBufferEdge->_tempBuffer->_tempBufferEdge = tempBufferEdge->_childEdge;

        IE_ASSERT(tempBufferEdge->_ptrPosInModel != _tempBufferEdgePtrList.end());
        _tempBufferEdgePtrList.erase(tempBufferEdge->_ptrPosInModel);
    }

    if (startInd >= 0) {
        IE_ASSERT(endInd > startInd && startInd <= parentStage->_tempBufferEdges.size());
        parentStage->_tempBufferEdges.erase(
            parentStage->_tempBufferEdges.begin() + startInd,
            parentStage->_tempBufferEdges.begin() + endInd);

        for (int i = 0; i < parentStage->_tempBufferEdges.size(); ++i) {
            parentStage->_tempBufferEdges[i]->_portInd = i;
        }
    }

    if (parentStage->_prevStages.empty()) {
        _initialStages.emplace(parentStage);
    } else {
        _initialStages.erase(parentStage);
    }

    if (childStage->_prevStages.empty()) {
        _initialStages.emplace(childStage);
    } else {
        _initialStages.erase(childStage);
    }

    //
    // Remove the InjectedStage Edge from the Model.
    //

    IE_ASSERT(edge->_ptrPosInModel != _stageEdgePtrList.end());
    _stageEdgePtrList.erase(edge->_ptrPosInModel);
}

Model::DataEdgeHelper::~DataEdgeHelper() {
    //
    // Check that `done` was called.
    //

    if (_model != nullptr) {
        std::terminate();
    }
}

Model::DataEdgeHelper& Model::DataEdgeHelper::parent(const Data& parent) {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that `parent` was not called.
    //

    IE_ASSERT(_parent == nullptr);

    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(parent->_model == _model);

    _parent = parent;

    return *this;
}

Model::DataEdgeHelper& Model::DataEdgeHelper::child(const Data& child) {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that `child` was not called.
    //

    IE_ASSERT(_child == nullptr);

    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(child->_model == _model);

    _child = child;

    return *this;
}

Model::DataEdgeHelper& Model::DataEdgeHelper::mode(SharedDataMode mode) {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that `mode` was not called.
    //

    IE_ASSERT(!_modeSet);

    _mode = mode;
    _modeSet = true;

    return *this;
}

Model::DataEdgeHelper& Model::DataEdgeHelper::order(SharedDataOrder order) {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that `order` was not called.
    //

    IE_ASSERT(!_orderSet);

    _order = order;
    _orderSet = true;

    return *this;
}

Model::DataEdgeHelper& Model::DataEdgeHelper::offset(const DimValues& offset) {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that `offset` was not called.
    //

    IE_ASSERT(!_offsetSet);

    _offset = offset;
    _offsetSet = true;

    return *this;
}

SharedAllocation Model::DataEdgeHelper::done() {
    //
    // Check that `done` was not called.
    //

    IE_ASSERT(_model != nullptr);

    //
    // Check that all fields were set.
    //

    IE_ASSERT(_parent != nullptr);
    IE_ASSERT(_child != nullptr);
    IE_ASSERT(_modeSet);
    IE_ASSERT(_orderSet);

    AutoScope autoNullModel([&] {
        _model = nullptr;
    });
    //
    // Call the actual implementation.
    //

    auto edge = _model->connectDatasImpl(
        _parent, _child,
        _mode, _order,
        _offset);

    //
    // Reset internal state.
    //

    _model = nullptr;

    return edge;
}

SharedAllocation Model::connectDatasImpl(
        const Data& parent,
        const Data& child,
        SharedDataMode mode,
        SharedDataOrder order,
        const DimValues& offset) {
    //
    // Get producer and consumer data.
    //

    Data producer, consumer;
    if (order == SharedDataOrder::ChildWritesToParent) {
        producer = child;
        consumer = parent;
    } else if (order == SharedDataOrder::ParentWritesToChild) {
        producer = parent;
        consumer = child;
    } else {
        VPU_THROW_EXCEPTION << "Invalid data order " << order;
    }

    //
    // Child must be Intermediate.
    //

    VPU_THROW_UNLESS(child->_usage == DataUsage::Intermediate);

    //
    // Parent can't be Temp or Fake.
    //

    VPU_THROW_UNLESS(parent->_usage != DataUsage::Temp && parent->_usage != DataUsage::Fake);

    //
    // Consumer must be accesible from the producer.
    //

    Stage connectionStage;

    for (const auto& consumerEdge : producer->_consumerEdges) {
        for (const auto& outEdge : consumerEdge->_consumer->_outputEdges) {
            if (outEdge->_output == consumer) {
                connectionStage = consumerEdge->_consumer;
                break;
            }
        }

        if (connectionStage != nullptr) {
            break;
        }
    }

    IE_ASSERT(connectionStage != nullptr);

    //
    // Connection stage must be special.
    //

    VPU_THROW_UNLESS(connectionStage->category() == StageCategory::Special);

    //
    // Special checks for each mode.
    //

    if (mode == SharedDataMode::ROI) {
        //
        // Check connection stage type and that parent has the largest buffer.
        //

        if (connectionStage->_type == StageType::Concat ||
            connectionStage->_type == StageType::Broadcast) {
            IE_ASSERT(producer == child);
            IE_ASSERT(consumer == parent);
        } else if (connectionStage->_type == StageType::Split ||
                   connectionStage->_type == StageType::Shrink) {
            IE_ASSERT(producer == parent);
            IE_ASSERT(consumer == child);
        } else {
            VPU_THROW_EXCEPTION
                    << "Stage type " << connectionStage->_type
                    << " can't be used for ROI data connection";
        }

        //
        // Parent and child must have the same order.
        //

        VPU_THROW_UNLESS(parent->desc().dimsOrder() == child->desc().dimsOrder());

        //
        // Offset must be valid.
        //

        for (const auto& p : offset) {
            IE_ASSERT(parent->desc().dimsOrder().hasDim(p.first));

            IE_ASSERT(child->desc().dim(p.first) + p.second <= parent->desc().dim(p.first));
        }

        //
        // Check strides requirements
        //

        IE_ASSERT(checkStrides(child->desc(), parent->strides(), child->_requiredStrides));
        child->resetRequiredStrides();
    } else if (mode == SharedDataMode::Reshape) {
        //
        // Check connection stage type.
        //

        IE_ASSERT(connectionStage->_type == StageType::Reshape);

        //
        // Parent and child must have the same data type.
        //

        IE_ASSERT(parent->desc().type() == child->desc().type());

        //
        // Parent and child must have the same number of elements.
        //

        IE_ASSERT(parent->desc().totalDimSize() == child->desc().totalDimSize());

        //
        // Parent and child must be compact.
        //

        // TODO: can we weaken this restriction?
        IE_ASSERT(parent->checkStrides(StridesRequirement::compact()));
        IE_ASSERT(child->checkStrides(StridesRequirement::compact()));
    } else {
        VPU_THROW_EXCEPTION << "Invalid shared data mode " << mode;
    }

    //
    // Remove previous edge if any.
    //

    auto prevEdge = child->_parentDataEdge;

    if (prevEdge != nullptr) {
        prevEdge->_parent->_childDataEdges.erase(prevEdge);
    }

    //
    // Create new Edge.
    //

    std::shared_ptr<SharedAllocationEdge> edge(new SharedAllocationEdge);

    edge->_parent = parent;
    edge->_child = child;
    edge->_connection = connectionStage;
    edge->_mode = mode;
    edge->_order = order;
    edge->_model = handle_from_this();
    if (mode == SharedDataMode::ROI) {
        edge->attrs().set<DimValues>("offset", offset);
    }

    edge->_ptrPosInModel = _dataEdgePtrList.emplace(_dataEdgePtrList.end(), edge);
    parent->_childDataEdges.push_back(edge);

    child->_parentDataEdge = edge;

    //
    // Deallocate previous edge if any.
    //

    if (prevEdge != nullptr) {
        IE_ASSERT(prevEdge->_ptrPosInModel != _dataEdgePtrList.end());
        _dataEdgePtrList.erase(prevEdge->_ptrPosInModel);
    }

    _allocator.setNeedToAllocNonIntermData();

    return edge;
}

void Model::disconnectStageDatas(const Stage& stage) {
    //
    // Check that objects belong to the same Model.
    //

    IE_ASSERT(stage->_model.get() == this);

    //
    // This affect the Stage order.
    //

    _resetStageOrder = true;;

    //
    // Disconnect input datas.
    //

    for (const auto& inEdge : stage->_inputEdges) {
        if (inEdge->_input->_producerEdge != nullptr) {
            auto it1 = inEdge->_input->_producerEdge->_producer->_nextStages.find(inEdge->_consumer);
            IE_ASSERT(it1 != inEdge->_input->_producerEdge->_producer->_nextStages.end());
            --it1->second;
            if (it1->second <= 0) {
                inEdge->_input->_producerEdge->_producer->_nextStages.erase(it1);
            }

            auto it2 = inEdge->_consumer->_prevStages.find(inEdge->_input->_producerEdge->_producer);
            IE_ASSERT(it2 != inEdge->_consumer->_prevStages.end());
            --it2->second;
            if (it2->second <= 0) {
                inEdge->_consumer->_prevStages.erase(it2);
            }
        }

        inEdge->_input->_consumerEdges.erase(inEdge);

        IE_ASSERT(inEdge->_ptrPosInModel != _inEdgePtrList.end());
        _inEdgePtrList.erase(inEdge->_ptrPosInModel);
    }

    stage->_inputEdges.clear();

    //
    // Disconnect output datas.
    //

    for (const auto& outEdge : stage->_outputEdges) {
        for (const auto& consumerEdge : outEdge->_output->_consumerEdges) {
            auto it1 = consumerEdge->_consumer->_prevStages.find(outEdge->_producer);
            IE_ASSERT(it1 != consumerEdge->_consumer->_prevStages.end());
            --it1->second;
            if (it1->second <= 0) {
                consumerEdge->_consumer->_prevStages.erase(it1);
            }

            auto it2 = outEdge->_producer->_nextStages.find(consumerEdge->_consumer);
            IE_ASSERT(it2 != outEdge->_producer->_nextStages.end());
            --it2->second;
            if (it2->second <= 0) {
                outEdge->_producer->_nextStages.erase(it2);
            }
        }

        outEdge->_output->_producerEdge = nullptr;

        IE_ASSERT(outEdge->_ptrPosInModel != _outEdgePtrList.end());
        _outEdgePtrList.erase(outEdge->_ptrPosInModel);
    }

    stage->_outputEdges.clear();

    //
    // Disconnect temp datas.
    //

    for (const auto& tempBufferEdge : stage->_tempBufferEdges) {
        tempBufferEdge->_tempBuffer->_tempBufferEdge = nullptr;

        IE_ASSERT(tempBufferEdge->_ptrPosInModel != _tempBufferEdgePtrList.end());
        _tempBufferEdgePtrList.erase(tempBufferEdge->_ptrPosInModel);
    }

    stage->_tempBufferEdges.clear();

    _allocator.setNeedToAllocNonIntermData();
}

void Model::removeStage(const Stage& stage) {
    IE_ASSERT(stage->_model.get() == this);

    _resetStageOrder = true;;

    disconnectStageDatas(stage);

    _initialStages.erase(stage);

    IE_ASSERT(stage->_ptrPosInModel != _stagePtrList.end());
    _stagePtrList.erase(stage->_ptrPosInModel);
}

void Model::cleanUpDatas() {
    bool needAllocatorPreprocess = false;

    for (const auto& data : datas()) {
        if (data->_usage == DataUsage::Input) {
            IE_ASSERT(!data->_consumerEdges.empty());
            IE_ASSERT(data->_parentDataEdge == nullptr);
        } else if (data->_usage == DataUsage::Output) {
            IE_ASSERT(data->_producerEdge != nullptr);
            IE_ASSERT(data->_parentDataEdge == nullptr);
        } else if (data->_usage == DataUsage::Temp) {
            if (data->_tempBufferEdge == nullptr) {
                _dataList.erase(data);

                IE_ASSERT(data->_ptrPosInModel != _dataPtrList.end());
                _dataPtrList.erase(data->_ptrPosInModel);
            }
        } else {
            if (data->_consumerEdges.empty() && data->_producerEdge == nullptr) {
                if (data->usage() != DataUsage::Intermediate) {
                    needAllocatorPreprocess = true;
                }

                _dataList.erase(data);

                IE_ASSERT(data->_ptrPosInModel != _dataPtrList.end());
                _dataPtrList.erase(data->_ptrPosInModel);
            }
        }
    }

    if (needAllocatorPreprocess) {
        _allocator.setNeedToAllocNonIntermData();
    }
}

void Model::buildStageOrder() const {
    if (!_resetStageOrder) {
        IE_ASSERT(_orderedStageList.size() == _stagePtrList.size());
        return;
    }

    VPU_PROFILE(buildStageOrder);

    _orderedStageList.clear();
    _resetStageOrder = false;

    if (_stagePtrList.empty()) {
        return;
    }

    //
    // Run recursive DFS algorithm
    //

    IE_ASSERT(!_initialStages.empty());

    StageMap<bool> visitedMap;
    for (const auto& stage : _initialStages) {
        runDFS(stage, visitedMap);
    }

    IE_ASSERT(_orderedStageList.size() == _stagePtrList.size());

    int stageInd = 0;
    for (const auto& stage : _orderedStageList) {
        stage->_index = stageInd;
        ++stageInd;
    }
}

void Model::runDFS(
        const Stage& stage,
        StageMap<bool>& visitedMap) const {
    IE_ASSERT(stage->_parentStageEdge == nullptr);

    visitedMap[stage] = false;

    for (const auto& nextStage : stage->_nextStages) {
        IE_ASSERT(nextStage.second > 0);

        auto it = visitedMap.find(nextStage.first);

        if (it != visitedMap.end()) {
            auto visited = it->second;

            if (!visited) {
                VPU_THROW_EXCEPTION << "Graph has cycle";
            }

            continue;
        }

        runDFS(nextStage.first, visitedMap);
    }

    visitedMap[stage] = true;

    _orderedStageList.push_front(stage);
}

Stage Model::addNewStageImpl(
    const std::string& name,
    StageType type,
    const ie::CNNLayerPtr& origLayer,
    const DataVector& inputs,
    const DataVector& outputs,
    const FuncRef<StagePtr()>& creator) {
    //
    // Check that Stage has inputs and outputs.
    //

    IE_ASSERT(!inputs.empty());
    IE_ASSERT(!outputs.empty());

    //
    // Check that Data objects belong to the same Model.
    //

    for (const auto& input : inputs) {
        IE_ASSERT(input->_model.get() == this);
    }
    for (const auto& output : outputs) {
        IE_ASSERT(output->_model.get() == this);
    }

    //
    // Check that there are no loops.
    //

    // TODO: more advanced check.
    for (const auto& output : outputs) {
        for (const auto& input : inputs) {
            IE_ASSERT(input != output);
        }
    }

    _resetStageOrder = true;;

    auto stage = creator();

    stage->_name = name;
    stage->_type = type;
    stage->_origLayer = origLayer;
    stage->_model = handle_from_this();

    for (const auto& input : inputs) {
        addStageInput(stage, input);
    }
    for (const auto& output : outputs) {
        addStageOutput(stage, output);
    }

    stage->_ptrPosInModel = _stagePtrList.emplace(_stagePtrList.end(), stage);

    return stage;
}

void Model::removeUnusedData(const Data& data) {
    IE_ASSERT(data->numConsumers() == 0);

    if (data->usage() != DataUsage::Intermediate &&
        data->usage() != DataUsage::Temp) {
        _allocator.setNeedToAllocNonIntermData();
    }

    _dataList.erase(data);

    IE_ASSERT(data->_ptrPosInModel != _dataPtrList.end());
    _dataPtrList.erase(data->_ptrPosInModel);
}

}  // namespace vpu
