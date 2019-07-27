// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "contracttx.h"

#include "entities/vote.h"
#include "commons/serialize.h"
#include "crypto/hash.h"
#include "main.h"
#include "miner/miner.h"
#include "persistence/contractdb.h"
#include "persistence/txdb.h"
#include "commons/util.h"
#include "config/version.h"
#include "vm/luavm/vmrunenv.h"

static bool GetKeyId(const CAccountDBCache &view, const string &userIdStr, CKeyID &KeyId) {
    switch (userIdStr.size()) {
    case 6:
        CRegID regId(userIdStr);
        KeyId = regId.GetKeyId(view);
        break;
    case 34:
        string addr(userIdStr.begin(), userIdStr.end());
        KeyId = CKeyID(addr);
        break;
    default:
        return false;
    }

    if (KeyId.IsEmpty())
        return false;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// class CContractDeployTx

bool CContractDeployTx::CheckTx(int height, CCacheWrapper &cw, CValidationState &state) {
    IMPLEMENT_CHECK_TX_FEE;
    IMPLEMENT_CHECK_TX_REGID(txUid.type());

    if (!contract.IsValid()) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, contract is invalid"),
                         REJECT_INVALID, "vmscript-invalid");
    }

    uint64_t llFuel = GetFuel(GetFuelRate(cw.contractCache));
    if (llFees < llFuel) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, fee too litter to afford fuel "
                         "(actual:%lld vs need:%lld)", llFees, llFuel),
                         REJECT_INVALID, "fee-too-litter-to-afford-fuel");
    }

    // If valid height range changed little enough(i.e. 3 blocks), remove it.
    if (GetFeatureForkVersion(height) == MAJOR_VER_R2) {
        unsigned int nTxSize = ::GetSerializeSize(SER_NETWORK, PROTOCOL_VERSION);
        double dFeePerKb     = double(llFees - llFuel) / (double(nTxSize) / 1000.0);
        if (dFeePerKb < CBaseTx::nMinRelayTxFee) {
            return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, fee too litter in fees/Kb "
                             "(actual:%.4f vs need:%lld)", dFeePerKb, CBaseTx::nMinRelayTxFee),
                             REJECT_INVALID, "fee-too-litter-in-fees/Kb");
        }
    }

    CAccount account;
    if (!cw.accountCache.GetAccount(txUid, account)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::CheckTx, get account failed"),
                         REJECT_INVALID, "bad-getaccount");
    }
    if (!account.HaveOwnerPubKey()) {
        return state.DoS(
            100, ERRORMSG("CContractDeployTx::CheckTx, account unregistered"),
            REJECT_INVALID, "bad-account-unregistered");
    }

    IMPLEMENT_CHECK_TX_SIGNATURE(account.owner_pubkey);

    return true;
}

bool CContractDeployTx::ExecuteTx(int height, int index, CCacheWrapper &cw, CValidationState &state) {
    CAccount account;
    if (!cw.accountCache.GetAccount(txUid, account)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, read regist addr %s account info error",
                        txUid.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
    }

    CAccount accountLog(account);
    if (!account.OperateBalance(SYMB::WICC, BalanceOpType::SUB_FREE, llFees)) {
            return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, operate account failed ,regId=%s",
                            txUid.ToString()), UPDATE_ACCOUNT_FAIL, "operate-account-failed");
    }

    if (!cw.accountCache.SetAccount(CUserID(account.keyid), account))
        return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, save account info error"),
            UPDATE_ACCOUNT_FAIL, "bad-save-accountdb");

    // create script account
    CAccount contractAccount;
    CRegID contractRegId(height, index);
    CKeyID keyId           = Hash160(contractRegId.GetRegIdRaw());
    contractAccount.keyid  = keyId;
    contractAccount.regid  = contractRegId;
    contractAccount.nickid = CNickID();

    // save new script content
    if (!cw.contractCache.SaveContract(contractRegId, CContract(LUA_VM, contract.code, "", contract.memo))) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, save code for contract id %s error",
                        contractRegId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }
    if (!cw.accountCache.SaveAccount(contractAccount)) {
        return state.DoS(100, ERRORMSG("CContractDeployTx::ExecuteTx, create new account script id %s script info error",
                        contractRegId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-scriptdb");
    }

    nRunStep = contract.GetContractSize();

    if (!SaveTxAddresses(height, index, cw, state, {txUid})) return false;

    return true;
}

bool CContractDeployTx::GetInvolvedKeyIds(CCacheWrapper &cw, set<CKeyID> &keyIds) {
    CKeyID keyId;
    if (!cw.accountCache.GetKeyId(txUid, keyId))
        return false;

    keyIds.insert(keyId);
    return true;
}

string CContractDeployTx::ToString(CAccountDBCache &view) {
    CKeyID keyId;
    view.GetKeyId(txUid, keyId);

    return strprintf("txType=%s, hash=%s, ver=%d, accountId=%s, keyid=%s, llFees=%ld, nValidHeight=%d\n",
                     GetTxType(nTxType), GetHash().ToString(), nVersion, txUid.ToString(), keyId.GetHex(),
                     llFees, nValidHeight);
}

Object CContractDeployTx::ToJson(const CAccountDBCache &accountCache) const {
    Object result;

    CKeyID keyid;
    accountCache.GetKeyId(txUid, keyid);

    IMPLEMENT_UNIVERSAL_ITEM_TO_JSON(accountCache)
    result.push_back(Pair("contract_code", contract.code));
    result.push_back(Pair("contract_memo", contract.memo));

    return result;
}

///////////////////////////////////////////////////////////////////////////////
// class CContractInvokeTx

bool CContractInvokeTx::GetInvolvedKeyIds(CCacheWrapper &cw, set<CKeyID> &keyIds) {
    CKeyID keyId;
    if (!cw.accountCache.GetKeyId(txUid, keyId))
        return false;

    keyIds.insert(keyId);
    CKeyID desKeyId;
    if (!cw.accountCache.GetKeyId(appUid, desKeyId))
        return false;

    keyIds.insert(desKeyId);

    //FIXME below:

    // CVmRunEnv vmRunEnv;
    // std::shared_ptr<CBaseTx> pTx = GetNewInstance();
    // uint64_t fuelRate = GetFuelRate(cw.contractCache);

    // if (!pCdMan->pTxCache->HaveTx(GetHash())) {
    //     tuple<bool, uint64_t, string> ret =
    //         vmRunEnv.ExecuteContract(pTx, chainActive.Height() + 1, cw, fuelRate, nRunStep);

    //     if (!std::get<0>(ret))
    //         return ERRORMSG("CContractInvokeTx::GetInvolvedKeyIds, %s", std::get<2>(ret));

    //     vector<shared_ptr<CAccount> > vpAccount = vmRunEnv.GetNewAccount();

    //     for (auto & item : vpAccount)
    //         keyIds.insert(item->keyid);

    //     vector<std::shared_ptr<CAppUserAccount> > &vAppUserAccount = vmRunEnv.GetRawAppUserAccount();
    //     for (auto & itemUserAccount : vAppUserAccount) {
    //         CKeyID itemKeyID;
    //         bool bValid = GetKeyId(cw.accountCache, itemUserAccount.get()->GetAccUserId(), itemKeyID);
    //         if (bValid)
    //             keyIds.insert(itemKeyID);
    //     }
    // } else {
    //     set<CKeyID> vTxRelAccount;
    //     if (!cw.contractCache.GetTxRelAccount(GetHash(), vTxRelAccount))
    //         return false;

    //     keyIds.insert(vTxRelAccount.begin(), vTxRelAccount.end());
    // }
    return true;
}

string CContractInvokeTx::ToString(CAccountDBCache &view) {
    return strprintf(
        "txType=%s, hash=%s, ver=%d, txUid=%s, appUid=%s, bcoins=%ld, llFees=%ld, arguments=%s, "
        "nValidHeight=%d\n",
        GetTxType(nTxType), GetHash().ToString(), nVersion, txUid.ToString(), appUid.ToString(), bcoins, llFees,
        HexStr(arguments), nValidHeight);
}

Object CContractInvokeTx::ToJson(const CAccountDBCache &accountView) const {
    Object result;
    CAccountDBCache view(accountView);

    // auto GetRegIdString = [&](CUserID const &userId) {
    //     if (userId.type() == typeid(CRegID))
    //         return userId.get<CRegID>().ToString();
    //     return string("");
    // };

    CKeyID srcKeyId, desKeyId;
    view.GetKeyId(txUid, srcKeyId);
    view.GetKeyId(appUid, desKeyId);

    result.push_back(Pair("txid",           GetHash().GetHex()));
    result.push_back(Pair("tx_type",        GetTxType(nTxType)));
    result.push_back(Pair("ver",            nVersion));
    result.push_back(Pair("regid",          txUid.ToString()));
    result.push_back(Pair("addr",           srcKeyId.ToAddress()));
    result.push_back(Pair("app_uid",        appUid.ToString()));
    result.push_back(Pair("app_addr",       desKeyId.ToAddress()));
    result.push_back(Pair("money",          bcoins));
    result.push_back(Pair("fees",           llFees));
    result.push_back(Pair("arguments",      HexStr(arguments)));
    result.push_back(Pair("valid_height",   nValidHeight));

    return result;
}

bool CContractInvokeTx::ExecuteTx(int height, int index, CCacheWrapper &cw, CValidationState &state) {
    CAccount srcAcct;
    CAccount desAcct;
    bool generateRegID = false;

    if (!cw.accountCache.GetAccount(txUid, srcAcct)) {
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, read source addr account info error"),
                         READ_ACCOUNT_FAIL, "bad-read-accountdb");
    } else {
        if (txUid.type() == typeid(CPubKey)) {
            srcAcct.owner_pubkey = txUid.get<CPubKey>();

            CRegID regId;
            // If the source account does NOT have CRegID, need to generate a new CRegID.
            if (!cw.accountCache.GetRegId(txUid, regId)) {
                srcAcct.regid = CRegID(height, index);
                generateRegID = true;
            }
        }
    }

    uint64_t minusValue = llFees + bcoins;
    if (!srcAcct.OperateBalance(SYMB::WICC, BalanceOpType::SUB_FREE, minusValue))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, accounts hash insufficient funds"),
            UPDATE_ACCOUNT_FAIL, "operate-minus-account-failed");

    if (generateRegID) {
        if (!cw.accountCache.SaveAccount(srcAcct))
            return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, save account info error"),
                             WRITE_ACCOUNT_FAIL, "bad-write-accountdb");
    } else {
        if (!cw.accountCache.SetAccount(CUserID(srcAcct.keyid), srcAcct))
            return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, save account info error"),
                             WRITE_ACCOUNT_FAIL, "bad-write-accountdb");
    }

    if (!cw.accountCache.GetAccount(appUid, desAcct)) {
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, get account info failed by regid:%s",
            appUid.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-accountdb");
    }

    if (!desAcct.OperateBalance(SYMB::WICC, BalanceOpType::ADD_FREE, bcoins)) {
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, operate accounts error"),
                        UPDATE_ACCOUNT_FAIL, "operate-add-account-failed");
    }

    if (!cw.accountCache.SetAccount(appUid, desAcct))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, save account error, kyeId=%s",
            desAcct.keyid.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-account");

    CContract contract;
    if (!cw.contractCache.GetContract(appUid.get<CRegID>(), contract))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, read script failed, regId=%s",
            appUid.get<CRegID>().ToString()), READ_ACCOUNT_FAIL, "bad-read-script");

    CVmRunEnv vmRunEnv;
    std::shared_ptr<CBaseTx> pTx = GetNewInstance();
    uint64_t fuelRate = GetFuelRate(cw.contractCache);

    int64_t llTime = GetTimeMillis();
    tuple<bool, uint64_t, string> ret = vmRunEnv.ExecuteContract(pTx, height, cw, fuelRate, nRunStep);
    if (!std::get<0>(ret))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, txid=%s run script error:%s",
            GetHash().GetHex(), std::get<2>(ret)), UPDATE_ACCOUNT_FAIL, "run-script-error: " + std::get<2>(ret));

    LogPrint("vm", "execute contract elapse: %lld, txid=%s\n", GetTimeMillis() - llTime, GetHash().GetHex());

    set<CKeyID> vAddress;
    CUserID userId;
    vector<std::shared_ptr<CAccount> > &vAccount = vmRunEnv.GetNewAccount();
    // Update accounts' info referred to the contract
    for (auto &itemAccount : vAccount) {
        vAddress.insert(itemAccount->keyid);
        userId = itemAccount->keyid;
        CAccount oldAcct;
        if (!cw.accountCache.GetAccount(userId, oldAcct)) {
            // The contract transfers money to an address for the first time.
            if (!itemAccount->keyid.IsNull()) {
                oldAcct.keyid = itemAccount->keyid;
            } else {
                return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, read account info error"),
                                 UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
            }
        }
        if (!cw.accountCache.SetAccount(userId, *itemAccount))
            return state.DoS(100, ERRORMSG("CContractInvokeTx::ExecuteTx, write account info error"),
                UPDATE_ACCOUNT_FAIL, "bad-write-accountdb");
    }

    vector<std::shared_ptr<CAppUserAccount> > &vAppUserAccount = vmRunEnv.GetRawAppUserAccount();
    for (auto & itemUserAccount : vAppUserAccount) {
        CKeyID itemKeyID;
        bool bValid = GetKeyId(cw.accountCache, itemUserAccount.get()->GetAccUserId(), itemKeyID);
        if (bValid) {
            vAddress.insert(itemKeyID);
        }
    }

    if (!cw.contractCache.SetTxRelAccout(GetHash(), vAddress))
        return ERRORMSG("CContractInvokeTx::ExecuteTx, save tx relate account info to script db error");

    if (!SaveTxAddresses(height, index, cw, state, {txUid, appUid})) return false;

    return true;
}

bool CContractInvokeTx::CheckTx(int height, CCacheWrapper &cw, CValidationState &state) {
    IMPLEMENT_CHECK_TX_FEE;
    IMPLEMENT_CHECK_TX_ARGUMENTS;
    IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid.type());
    IMPLEMENT_CHECK_TX_APPID(appUid.type());

    if ((txUid.type() == typeid(CPubKey)) && !txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CContractInvokeTx::CheckTx, public key is invalid"), REJECT_INVALID,
                         "bad-publickey");

    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::CheckTx, read account failed, regId=%s",
                        txUid.get<CRegID>().ToString()), REJECT_INVALID, "bad-getaccount");

    if (!srcAccount.HaveOwnerPubKey())
        return state.DoS(100, ERRORMSG("CContractInvokeTx::CheckTx, account unregistered"),
                        REJECT_INVALID, "bad-account-unregistered");

    CContract contract;
    if (!cw.contractCache.GetContract(appUid.get<CRegID>(), contract))
        return state.DoS(100, ERRORMSG("CContractInvokeTx::CheckTx, read script failed, regId=%s",
                        appUid.get<CRegID>().ToString()), REJECT_INVALID, "bad-read-script");

    CPubKey pubKey = (txUid.type() == typeid(CPubKey) ? txUid.get<CPubKey>() : srcAccount.owner_pubkey);
    IMPLEMENT_CHECK_TX_SIGNATURE(pubKey);

    return true;
}