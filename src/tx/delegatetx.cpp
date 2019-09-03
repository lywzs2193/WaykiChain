// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2017-2019 The WaykiChain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include "delegatetx.h"

#include "commons/serialize.h"
#include "tx.h"
#include "crypto/hash.h"
#include "commons/util.h"
#include "main.h"
#include "vm/luavm/luavmrunenv.h"
#include "miner/miner.h"
#include "config/version.h"

bool CDelegateVoteTx::CheckTx(int height, CCacheWrapper &cw, CValidationState &state) {
    IMPLEMENT_CHECK_TX_FEE;
    IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(txUid.type());

    if (candidateVotes.empty() || candidateVotes.size() > IniCfg().GetMaxVoteCandidateNum()) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::CheckTx, candidate votes out of range"), REJECT_INVALID,
                         "candidate-votes-out-of-range");
    }

    if ((txUid.type() == typeid(CPubKey)) && !txUid.get<CPubKey>().IsFullyValid())
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::CheckTx, public key is invalid"), REJECT_INVALID,
                        "bad-publickey");

    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount)) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::CheckTx, get account info error, userid=%s", txUid.ToString()),
                         REJECT_INVALID, "bad-read-accountdb");
    }

    if (GetFeatureForkVersion(height) == MAJOR_VER_R2) {
        CPubKey pubKey = (txUid.type() == typeid(CPubKey) ? txUid.get<CPubKey>() : srcAccount.owner_pubkey);
        IMPLEMENT_CHECK_TX_SIGNATURE(pubKey);
    }

    // check candidate duplication
    set<CKeyID> voteKeyIds;
    for (const auto &vote : candidateVotes) {
        // candidate uid should be CPubKey or CRegID
        IMPLEMENT_CHECK_TX_REGID_OR_PUBKEY(vote.GetCandidateUid().type());

        if (0 >= vote.GetVotedBcoins() || (uint64_t)GetBaseCoinMaxMoney() < vote.GetVotedBcoins())
            return ERRORMSG("CDelegateVoteTx::CheckTx, votes: %lld not within (0 .. MaxVote)", vote.GetVotedBcoins());

        CAccount account;
        if (!cw.accountCache.GetAccount(vote.GetCandidateUid(), account))
            return state.DoS(100, ERRORMSG("CDelegateVoteTx::CheckTx, get account info error, address=%s",
                             vote.GetCandidateUid().ToString()), REJECT_INVALID, "bad-read-accountdb");
        if (vote.GetCandidateUid().type() == typeid(CPubKey)) {
            voteKeyIds.insert(vote.GetCandidateUid().get<CPubKey>().GetKeyId());
        } else {  // vote.GetCandidateUid().type() == typeid(CRegID)
            voteKeyIds.insert(account.keyid);
        }

        if (GetFeatureForkVersion(height) == MAJOR_VER_R2) {
            if (!account.HaveOwnerPubKey()) {
                return state.DoS(100, ERRORMSG("CDelegateVoteTx::CheckTx, account is unregistered, address=%s",
                                 vote.GetCandidateUid().ToString()), REJECT_INVALID, "bad-read-accountdb");
            }
        }
    }

    if (voteKeyIds.size() != candidateVotes.size()) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::CheckTx, duplication candidate"), REJECT_INVALID,
                         "duplication-candidate-error");
    }

    return true;
}

bool CDelegateVoteTx::ExecuteTx(int height, int index, CCacheWrapper &cw, CValidationState &state) {
    CAccount srcAccount;
    if (!cw.accountCache.GetAccount(txUid, srcAccount)) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, read account info error"), UPDATE_ACCOUNT_FAIL,
                         "bad-read-accountdb");
    }

    if (!GenerateRegID(srcAccount, cw, state, height, index)) {
        return false;
    }

    if (!srcAccount.OperateBalance(SYMB::WICC, SUB_FREE, llFees)) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, operate account failed, txUid=%s",
                        txUid.ToString()), UPDATE_ACCOUNT_FAIL, "operate-account-failed");
    }

    vector<CCandidateReceivedVote> candidateVotesInOut;
    CRegID &regId = srcAccount.regid;
    cw.delegateCache.GetCandidateVotes(regId, candidateVotesInOut);

    vector<CReceipt> receipts;
    if (!srcAccount.ProcessDelegateVotes(candidateVotes, candidateVotesInOut, height, cw.accountCache, receipts)) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, operate delegate vote failed, txUid=%s",
                        txUid.ToString()), UPDATE_ACCOUNT_FAIL, "operate-delegate-failed");
    }
    if (!cw.delegateCache.SetCandidateVotes(regId, candidateVotesInOut)) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, write candidate votes failed, txUid=%s", txUid.ToString()),
                        WRITE_CANDIDATE_VOTES_FAIL, "write-candidate-votes-failed");
    }

    if (!cw.accountCache.SaveAccount(srcAccount)) {
        return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, save account info error"), UPDATE_ACCOUNT_FAIL,
                         "bad-save-accountdb");
    }

    for (const auto &vote : candidateVotes) {
        CAccount delegate;
        const CUserID &delegateUId = vote.GetCandidateUid();
        if (!cw.accountCache.GetAccount(delegateUId, delegate)) {
            return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, read account id %s account info error",
                            delegateUId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-read-accountdb");
        }
        uint64_t oldVotes = delegate.received_votes;
        if (!delegate.StakeVoteBcoins(VoteType(vote.GetCandidateVoteType()), vote.GetVotedBcoins())) {
            return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, operate account id %s vote fund error",
                            delegateUId.ToString()), UPDATE_ACCOUNT_FAIL, "operate-vote-error");
        }

        // Votes: set the new value and erase the old value
        if (!cw.delegateCache.SetDelegateVotes(delegate.regid, delegate.received_votes)) {
            return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, save account id %s vote info error",
                            delegateUId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-delegatedb");
        }

        if (!cw.delegateCache.EraseDelegateVotes(delegate.regid, oldVotes)) {
            return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, erase account id %s vote info error",
                            delegateUId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-delegatedb");
        }

        if (!cw.accountCache.SaveAccount(delegate)) {
            return state.DoS(100, ERRORMSG("CDelegateVoteTx::ExecuteTx, save account id %s info error",
                            delegateUId.ToString()), UPDATE_ACCOUNT_FAIL, "bad-save-accountdb");
        }
    }

    cw.txReceiptCache.SetTxReceipts(GetHash(), receipts);

    return true;
}

string CDelegateVoteTx::ToString(CAccountDBCache &accountCache) {
    string str;

    str += strprintf("txType=%s, hash=%s, ver=%d, txUid=%s, llFees=%llu, valid_height=%d", GetTxType(nTxType),
                     GetHash().ToString(), nVersion, txUid.ToString(), llFees, valid_height);
    str += "vote: ";
    for (const auto &vote : candidateVotes) {
        str += strprintf("%s", vote.ToString());
    }

    return str;
}

Object CDelegateVoteTx::ToJson(const CAccountDBCache &accountCache) const {
    Object result = CBaseTx::ToJson(accountCache);

    Array candidateVoteArray;
    for (const auto &vote : candidateVotes) {
        candidateVoteArray.push_back(vote.ToJson());
    }

    result.push_back(Pair("candidate_votes",    candidateVoteArray));
    return result;
}