// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018-2022 The Veco Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "spork.h"

#include "base58.h"
#include "chainparams.h"
#include "validation.h"
#include "messagesigner.h"
#include "net_processing.h"
#include "netmessagemaker.h"

#include <string>

CSporkManager sporkManager;

const std::string CSporkManager::SERIALIZATION_VERSION_STRING = "CSporkManager-Version-2";

std::map<int, int64_t> mapSporkDefaults = {
    {SPORK_2_INSTANTSEND_ENABLED,            0},             // ON
    {SPORK_3_INSTANTSEND_BLOCK_FILTERING,    0},             // ON
    {SPORK_5_INSTANTSEND_MAX_VALUE,          1000},          // 1000 VECO
    {SPORK_6_NEW_SIGS,                       4070908800ULL}, // OFF
    {SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, 1540166400},    // ON
    {SPORK_9_SUPERBLOCKS_ENABLED,            4070908800ULL}, // OFF
    {SPORK_10_MASTERNODE_PAY_UPDATED_NODES,  4070908800ULL}, // OFF
    {SPORK_12_RECONSIDER_BLOCKS,             0},             // 0 BLOCKS
    {SPORK_14_REQUIRE_SENTINEL_FLAG,         4070908800ULL}, // OFF
    {SPORK_15_DETERMINISTIC_MNS_ENABLED,     1384000},    // ON
    {SPORK_16_INSTANTSEND_AUTOLOCKS,         4070908800ULL}, // OFF
    {SPORK_17_QUORUM_DKG_ENABLED,            4070908800ULL}, // OFF
};

bool CSporkManager::SporkValueIsActive(int nSporkID, int64_t &nActiveValueRet) const
{
    LOCK(cs);

    if (!mapSporksActive.count(nSporkID)) return false;

    // calc how many values we have and how many signers vote for every value
    std::map<int64_t, int> mapValueCounts;
    for (const auto& pair: mapSporksActive.at(nSporkID)) {
        mapValueCounts[pair.second.nValue]++;
        if (mapValueCounts.at(pair.second.nValue) >= nMinSporkKeys) {
            // nMinSporkKeys is always more than the half of the max spork keys number,
            // so there is only one such value and we can stop here
            nActiveValueRet = pair.second.nValue;
            return true;
        }
    }

    return false;
}

void CSporkManager::Clear()
{
    LOCK(cs);
    mapSporksActive.clear();
    mapSporksByHash.clear();
    // sporkPubKeyID and sporkPrivKey should be set in init.cpp,
    // we should not alter them here.
}

void CSporkManager::CheckAndRemove()
{
    LOCK(cs);
    bool fSporkAddressIsSet = !setSporkPubKeyIDs.empty();
    assert(fSporkAddressIsSet);

    auto itActive = mapSporksActive.begin();
    while (itActive != mapSporksActive.end()) {
        auto itSignerPair = itActive->second.begin();
        while (itSignerPair != itActive->second.end()) {
            if (setSporkPubKeyIDs.find(itSignerPair->first) == setSporkPubKeyIDs.end()) {
                mapSporksByHash.erase(itSignerPair->second.GetHash());
                continue;
            }
            if (!itSignerPair->second.CheckSignature(itSignerPair->first, false)) {
                if (!itSignerPair->second.CheckSignature(itSignerPair->first, true)) {
                    mapSporksByHash.erase(itSignerPair->second.GetHash());
                    itActive->second.erase(itSignerPair++);
                    continue;
                }
            }
            ++itSignerPair;
        }
        if (itActive->second.empty()) {
            mapSporksActive.erase(itActive++);
            continue;
        }
        ++itActive;
    }

    auto itByHash = mapSporksByHash.begin();
    while (itByHash != mapSporksByHash.end()) {
        bool found = false;
        for (const auto& signer: setSporkPubKeyIDs) {
            if (itByHash->second.CheckSignature(signer, false) ||
                itByHash->second.CheckSignature(signer, true)) {
                found = true;
                break;
            }
        }
        if (!found) {
            mapSporksByHash.erase(itByHash++);
            continue;
        }
        ++itByHash;
    }
}

void CSporkManager::ProcessSpork(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all Veco specific functionality

    if (strCommand == NetMsgType::SPORK) {

        CSporkMessage spork;
        vRecv >> spork;

        uint256 hash = spork.GetHash();

        std::string strLogMsg;
        {
            LOCK(cs_main);
            connman.RemoveAskFor(hash);
            if(!chainActive.Tip()) return;
            strLogMsg = strprintf("SPORK -- hash: %s id: %d value: %10d bestHeight: %d peer=%d", hash.ToString(), spork.nSporkID, spork.nValue, chainActive.Height(), pfrom->id);
        }

        if (spork.nTimeSigned > GetAdjustedTime() + 2 * 60 * 60) {
            LOCK(cs_main);
            LogPrintf("CSporkManager::ProcessSpork -- ERROR: too far into the future\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        CKeyID keyIDSigner;
        bool fSpork6IsActive = IsSporkActive(SPORK_6_NEW_SIGS);
        if (!spork.GetSignerKeyID(keyIDSigner, fSpork6IsActive)
                                 || !setSporkPubKeyIDs.count(keyIDSigner)) {
            // Note: unlike for other messages we have to check for new format even with SPORK_6_NEW_SIGS
            // inactive because SPORK_6_NEW_SIGS default is OFF and it is not the first spork to sync
            // (and even if it would, spork order can't be guaranteed anyway).
            if (!spork.GetSignerKeyID(keyIDSigner, !fSpork6IsActive)
                                     || !setSporkPubKeyIDs.count(keyIDSigner)) {
                LOCK(cs_main);
                LogPrintf("CSporkManager::ProcessSpork -- ERROR: invalid signature\n");
                Misbehaving(pfrom->GetId(), 100);
                return;
            }
        }

        {
            LOCK(cs); // make sure to not lock this together with cs_main
            if (mapSporksActive.count(spork.nSporkID)) {
                if (mapSporksActive[spork.nSporkID].count(keyIDSigner)) {
                    if (mapSporksActive[spork.nSporkID][keyIDSigner].nTimeSigned >= spork.nTimeSigned) {
                        LogPrint("spork", "%s seen\n", strLogMsg);
                        return;
                    } else {
                        LogPrintf("%s updated\n", strLogMsg);
                    }
                } else {
                    LogPrintf("%s new signer\n", strLogMsg);
                }
            } else {
                LogPrintf("%s new\n", strLogMsg);
            }
        }


        {
            LOCK(cs); // make sure to not lock this together with cs_main
            mapSporksByHash[hash] = spork;
            mapSporksActive[spork.nSporkID][keyIDSigner] = spork;
        }
        spork.Relay(connman);

        //does a task if needed
        int64_t nActiveValue = 0;
        if (SporkValueIsActive(spork.nSporkID, nActiveValue)) {
            ExecuteSpork(spork.nSporkID, nActiveValue);
        }

    } else if (strCommand == NetMsgType::GETSPORKS) {
        LOCK(cs); // make sure to not lock this together with cs_main
        for (const auto& pair : mapSporksActive) {
            for (const auto& signerSporkPair: pair.second) {
                connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SPORK, signerSporkPair.second));
            }
        }
    }

}

void CSporkManager::ExecuteSpork(int nSporkID, int nValue)
{
    //correct fork via spork technology
    if(nSporkID == SPORK_12_RECONSIDER_BLOCKS && nValue > 0) {
        // allow to reprocess 24h of blocks max, which should be enough to resolve any issues
        int64_t nMaxBlocks = 576;
        // this potentially can be a heavy operation, so only allow this to be executed once per 10 minutes
        int64_t nTimeout = 10 * 60;

        static int64_t nTimeExecuted = 0; // i.e. it was never executed before

        if(GetTime() - nTimeExecuted < nTimeout) {
            LogPrint("spork", "CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider blocks, too soon - %d/%d\n", GetTime() - nTimeExecuted, nTimeout);
            return;
        }

        if(nValue > nMaxBlocks) {
            LogPrintf("CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider too many blocks %d/%d\n", nValue, nMaxBlocks);
            return;
        }


        LogPrintf("CSporkManager::ExecuteSpork -- Reconsider Last %d Blocks\n", nValue);

        ReprocessBlocks(nValue);
        nTimeExecuted = GetTime();
    }
}

bool CSporkManager::UpdateSpork(int nSporkID, int64_t nValue, CConnman& connman)
{
    CSporkMessage spork = CSporkMessage(nSporkID, nValue, GetAdjustedTime());

    bool fSpork6IsActive = IsSporkActive(SPORK_6_NEW_SIGS);
    if(spork.Sign(sporkPrivKey, fSpork6IsActive)) {
        CKeyID keyIDSigner;
        if (!spork.GetSignerKeyID(keyIDSigner, fSpork6IsActive) || !setSporkPubKeyIDs.count(keyIDSigner)) {
            LogPrintf("CSporkManager::UpdateSpork: failed to find keyid for private key\n");
            return false;
        }
        spork.Relay(connman);
        LOCK(cs);
        mapSporksByHash[spork.GetHash()] = spork;
        mapSporksActive[nSporkID][keyIDSigner] = spork;
        return true;
    }

    return false;
}

bool CSporkManager::IsSporkActive(int nSporkID)
{
    LOCK(cs);
    int64_t nSporkValue = -1;

    if (SporkValueIsActive(nSporkID, nSporkValue)) {
        return nSporkValue < GetAdjustedTime();
    }

    if (mapSporkDefaults.count(nSporkID)) {
        return  mapSporkDefaults[nSporkID] < GetAdjustedTime();
    }

    LogPrint("spork", "CSporkManager::IsSporkActive -- Unknown Spork ID %d\n", nSporkID);
    return false;
}

int64_t CSporkManager::GetSporkValue(int nSporkID)
{
    LOCK(cs);

    int64_t nSporkValue = -1;
    if (SporkValueIsActive(nSporkID, nSporkValue)) {
        return nSporkValue;
    }

    if (mapSporkDefaults.count(nSporkID)) {
        return mapSporkDefaults[nSporkID];
    }

    LogPrint("spork", "CSporkManager::GetSporkValue -- Unknown Spork ID %d\n", nSporkID);
    return -1;
}

int CSporkManager::GetSporkIDByName(const std::string& strName)
{
    if (strName == "SPORK_2_INSTANTSEND_ENABLED")               return SPORK_2_INSTANTSEND_ENABLED;
    if (strName == "SPORK_3_INSTANTSEND_BLOCK_FILTERING")       return SPORK_3_INSTANTSEND_BLOCK_FILTERING;
    if (strName == "SPORK_5_INSTANTSEND_MAX_VALUE")             return SPORK_5_INSTANTSEND_MAX_VALUE;
    if (strName == "SPORK_6_NEW_SIGS")                          return SPORK_6_NEW_SIGS;
    if (strName == "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT")    return SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT;
    if (strName == "SPORK_9_SUPERBLOCKS_ENABLED")               return SPORK_9_SUPERBLOCKS_ENABLED;
    if (strName == "SPORK_10_MASTERNODE_PAY_UPDATED_NODES")     return SPORK_10_MASTERNODE_PAY_UPDATED_NODES;
    if (strName == "SPORK_12_RECONSIDER_BLOCKS")                return SPORK_12_RECONSIDER_BLOCKS;
    if (strName == "SPORK_14_REQUIRE_SENTINEL_FLAG")            return SPORK_14_REQUIRE_SENTINEL_FLAG;
    if (strName == "SPORK_15_DETERMINISTIC_MNS_ENABLED")        return SPORK_15_DETERMINISTIC_MNS_ENABLED;
    if (strName == "SPORK_16_INSTANTSEND_AUTOLOCKS")            return SPORK_16_INSTANTSEND_AUTOLOCKS;
    if (strName == "SPORK_17_QUORUM_DKG_ENABLED")               return SPORK_17_QUORUM_DKG_ENABLED;

    LogPrint("spork", "CSporkManager::GetSporkIDByName -- Unknown Spork name '%s'\n", strName);
    return -1;
}

std::string CSporkManager::GetSporkNameByID(int nSporkID)
{
    switch (nSporkID) {
        case SPORK_2_INSTANTSEND_ENABLED:               return "SPORK_2_INSTANTSEND_ENABLED";
        case SPORK_3_INSTANTSEND_BLOCK_FILTERING:       return "SPORK_3_INSTANTSEND_BLOCK_FILTERING";
        case SPORK_5_INSTANTSEND_MAX_VALUE:             return "SPORK_5_INSTANTSEND_MAX_VALUE";
        case SPORK_6_NEW_SIGS:                          return "SPORK_6_NEW_SIGS";
        case SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT:    return "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT";
        case SPORK_9_SUPERBLOCKS_ENABLED:               return "SPORK_9_SUPERBLOCKS_ENABLED";
        case SPORK_10_MASTERNODE_PAY_UPDATED_NODES:     return "SPORK_10_MASTERNODE_PAY_UPDATED_NODES";
        case SPORK_12_RECONSIDER_BLOCKS:                return "SPORK_12_RECONSIDER_BLOCKS";
        case SPORK_14_REQUIRE_SENTINEL_FLAG:            return "SPORK_14_REQUIRE_SENTINEL_FLAG";
        case SPORK_15_DETERMINISTIC_MNS_ENABLED:        return "SPORK_15_DETERMINISTIC_MNS_ENABLED";
        case SPORK_16_INSTANTSEND_AUTOLOCKS:            return "SPORK_16_INSTANTSEND_AUTOLOCKS";
        case SPORK_17_QUORUM_DKG_ENABLED:               return "SPORK_17_QUORUM_DKG_ENABLED";
        default:
            LogPrint("spork", "CSporkManager::GetSporkNameByID -- Unknown Spork ID %d\n", nSporkID);
            return "Unknown";
    }
}

bool CSporkManager::GetSporkByHash(const uint256& hash, CSporkMessage &sporkRet)
{
    LOCK(cs);

    const auto it = mapSporksByHash.find(hash);

    if (it == mapSporksByHash.end())
        return false;

    sporkRet = it->second;

    return true;
}

bool CSporkManager::SetSporkAddress(const std::string& strAddress) {
    LOCK(cs);
    CBitcoinAddress address(strAddress);
    CKeyID keyid;
    if (!address.IsValid() || !address.GetKeyID(keyid)) {
        LogPrintf("CSporkManager::SetSporkAddress -- Failed to parse spork address\n");
        return false;
    }
    setSporkPubKeyIDs.insert(keyid);
    return true;
}

bool CSporkManager::SetMinSporkKeys(int minSporkKeys)
{
    int maxKeysNumber = setSporkPubKeyIDs.size();
    if ((minSporkKeys <= maxKeysNumber / 2) || (minSporkKeys > maxKeysNumber)) {
        LogPrintf("CSporkManager::SetMinSporkKeys -- Invalid min spork signers number: %d\n", minSporkKeys);
        return false;
    }
    nMinSporkKeys = minSporkKeys;
    return true;
}

bool CSporkManager::SetPrivKey(const std::string& strPrivKey)
{
    CKey key;
    CPubKey pubKey;
    if(!CMessageSigner::GetKeysFromSecret(strPrivKey, key, pubKey)) {
        LogPrintf("CSporkManager::SetPrivKey -- Failed to parse private key\n");
        return false;
    }

    if (setSporkPubKeyIDs.find(pubKey.GetID()) == setSporkPubKeyIDs.end()) {
        LogPrintf("CSporkManager::SetPrivKey -- New private key does not belong to spork addresses\n");
        return false;
    }

    CSporkMessage spork;
    if (spork.Sign(key, IsSporkActive(SPORK_6_NEW_SIGS))) {
	    LOCK(cs);
        // Test signing successful, proceed
        LogPrintf("CSporkManager::SetPrivKey -- Successfully initialized as spork signer\n");

        sporkPrivKey = key;
        return true;
    } else {
        LogPrintf("CSporkManager::SetPrivKey -- Test signing failed\n");
        return false;
    }
}

std::string CSporkManager::ToString() const
{
    LOCK(cs);
    return strprintf("Sporks: %llu", mapSporksActive.size());
}

uint256 CSporkMessage::GetHash() const
{
    return SerializeHash(*this);
}

uint256 CSporkMessage::GetSignatureHash() const
{
    CHashWriter s(SER_GETHASH, 0);
    s << nSporkID;
    s << nValue;
    s << nTimeSigned;
    return s.GetHash();
}

bool CSporkMessage::Sign(const CKey& key, bool fSporkSixActive)
{
    if (!key.IsValid()) {
        LogPrintf("CSporkMessage::Sign -- signing key is not valid\n");
        return false;
    }

    CKeyID pubKeyId = key.GetPubKey().GetID();
    std::string strError = "";

    if (fSporkSixActive) {
        uint256 hash = GetSignatureHash();

        if(!CHashSigner::SignHash(hash, key, vchSig)) {
            LogPrintf("CSporkMessage::Sign -- SignHash() failed\n");
            return false;
        }

        if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig, strError)) {
            LogPrintf("CSporkMessage::Sign -- VerifyHash() failed, error: %s\n", strError);
            return false;
        }
    } else {
        std::string strMessage = std::to_string(nSporkID) + std::to_string(nValue) + std::to_string(nTimeSigned);

        if(!CMessageSigner::SignMessage(strMessage, vchSig, key)) {
            LogPrintf("CSporkMessage::Sign -- SignMessage() failed\n");
            return false;
        }

        if(!CMessageSigner::VerifyMessage(pubKeyId, vchSig, strMessage, strError)) {
            LogPrintf("CSporkMessage::Sign -- VerifyMessage() failed, error: %s\n", strError);
            return false;
        }
    }

    return true;
}

bool CSporkMessage::CheckSignature(const CKeyID& pubKeyId, bool fSporkSixActive) const
{
    std::string strError = "";

    if (fSporkSixActive) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig, strError)) {
            // Note: unlike for many other messages when SPORK_6_NEW_SIGS is ON sporks with sigs in old format
            // and newer timestamps should not be accepted, so if we failed here - that's it
            LogPrintf("CSporkMessage::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
            return false;
        }
    } else {
        std::string strMessage = std::to_string(nSporkID) + std::to_string(nValue) + std::to_string(nTimeSigned);

        if (!CMessageSigner::VerifyMessage(pubKeyId, vchSig, strMessage, strError)){
            // Note: unlike for other messages we have to check for new format even with SPORK_6_NEW_SIGS
            // inactive because SPORK_6_NEW_SIGS default is OFF and it is not the first spork to sync
            // (and even if it would, spork order can't be guaranteed anyway).
            uint256 hash = GetSignatureHash();
            if (!CHashSigner::VerifyHash(hash, pubKeyId, vchSig, strError)) {
                LogPrintf("CSporkMessage::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
                return false;
            }
        }
    }

    return true;
}

bool CSporkMessage::GetSignerKeyID(CKeyID &retKeyidSporkSigner, bool fSporkSixActive)
{
    CPubKey pubkeyFromSig;
    if (fSporkSixActive) {
        if (!pubkeyFromSig.RecoverCompact(GetSignatureHash(), vchSig)) {
            return false;
        }
    } else {
        std::string strMessage = std::to_string(nSporkID) + std::to_string(nValue) + std::to_string(nTimeSigned);
        CHashWriter ss(SER_GETHASH, 0);
        ss << strMessageMagic;
        ss << strMessage;
        if (!pubkeyFromSig.RecoverCompact(ss.GetHash(), vchSig)) {
            return false;
        }
    }

    retKeyidSporkSigner = pubkeyFromSig.GetID();
    return true;
}

void CSporkMessage::Relay(CConnman& connman)
{
    CInv inv(MSG_SPORK, GetHash());
    connman.RelayInv(inv);
}
