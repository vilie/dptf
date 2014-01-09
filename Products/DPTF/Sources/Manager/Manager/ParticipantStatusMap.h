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
#pragma once
#include "Dptf.h"

class XmlNode;
class ParticipantManager;

class ParticipantStatusMap
{
public:

    ParticipantStatusMap(ParticipantManager* dptfManager);

    std::string getGroupsString();
    XmlNode* getStatusAsXml(UIntN mappedIndex);
    void clearCachedData();

private:

    ParticipantManager* m_participantManager;
    std::vector<std::pair<UIntN, UIntN>> m_participantDomainsList;
    void buildParticipantDomainsList();
};