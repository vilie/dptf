/******************************************************************************
** Copyright (c) 2013 Intel Corporation All Rights Reserved
**
** Licensed under the Apache License, Version 2.0 (the "License"); you may not
** use this file except in compliance with the License.
**
** You may obtain a copy of the License at
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**
** See the License for the specific language governing permissions and
** limitations under the License.
**
******************************************************************************/
#include "PolicyServicesDomainPowerControl.h"
#include "ParticipantManager.h"

PolicyServicesDomainPowerControl::PolicyServicesDomainPowerControl(DptfManager* dptfManager, UIntN policyIndex) :
    PolicyServices(dptfManager, policyIndex)
{
}

PowerControlDynamicCapsSet PolicyServicesDomainPowerControl::getPowerControlDynamicCapsSet(
    UIntN participantIndex, UIntN domainIndex)
{
    throwIfNotWorkItemThread();
    return getParticipantManager()->getParticipantPtr(participantIndex)->getPowerControlDynamicCapsSet(domainIndex);
}

PowerControlStatusSet PolicyServicesDomainPowerControl::getPowerControlStatusSet(
    UIntN participantIndex, UIntN domainIndex)
{
    throwIfNotWorkItemThread();
    return getParticipantManager()->getParticipantPtr(participantIndex)->getPowerControlStatusSet(domainIndex);
}

void PolicyServicesDomainPowerControl::setPowerControl(UIntN participantIndex, UIntN domainIndex,
    const PowerControlStatusSet& powerControlStatusSet)
{
    throwIfNotWorkItemThread();
    getParticipantManager()->getParticipantPtr(participantIndex)->setPowerControl(domainIndex,
        getPolicyIndex(), powerControlStatusSet);
}