#include "fd_executor.h"
#include "fd_acc_mgr.h"
#include "fd_hashes.h"
#include "fd_runtime_err.h"
#include "context/fd_exec_slot_ctx.h"
#include "context/fd_exec_txn_ctx.h"
#include "context/fd_exec_instr_ctx.h"

#include "../nanopb/pb_encode.h"
#include "../../util/rng/fd_rng.h"
#include "fd_system_ids.h"
#include "fd_account.h"
#include "program/fd_address_lookup_table_program.h"
#include "program/fd_bpf_loader_program.h"
#include "program/fd_compute_budget_program.h"
#include "program/fd_config_program.h"
#include "program/fd_precompiles.h"
#include "program/fd_stake_program.h"
#include "program/fd_system_program.h"
#include "program/fd_vote_program.h"
#include "program/fd_zk_elgamal_proof_program.h"
#include "sysvar/fd_sysvar_slot_history.h"

#include "sysvar/fd_sysvar_instructions.h"

#include "../../ballet/base58/fd_base58.h"
#include "../../ballet/pack/fd_pack.h"
#include "../../ballet/pack/fd_pack_cost.h"

#define SORT_NAME        sort_uint64_t
#define SORT_KEY_T       uint64_t
#define SORT_BEFORE(a,b) (a)<(b)
#include "../../util/tmpl/fd_sort.c"

#include <assert.h>
#include <errno.h>
#include <stdio.h>   /* snprintf(3) */
#include <fcntl.h>   /* openat(2) */
#include <unistd.h>  /* write(3) */
#include <time.h>

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"

#define MAX_COMPUTE_UNITS_PER_BLOCK                (48000000UL)
#define MAX_COMPUTE_UNITS_PER_WRITE_LOCKED_ACCOUNT (12000000UL)

/* https://github.com/anza-xyz/agave/blob/9efdd74b1b65ecfd85b0db8ad341c6bd4faddfef/program-runtime/src/invoke_context.rs#L461-L488 */
fd_exec_instr_fn_t
fd_executor_lookup_native_program( fd_borrowed_account_t const * prog_acc ) {
  fd_pubkey_t const * pubkey        = prog_acc->pubkey;
  fd_pubkey_t const * owner         = (fd_pubkey_t const *)prog_acc->const_meta->info.owner;
  fd_pubkey_t const * lookup_pubkey = !memcmp( owner, fd_solana_native_loader_id.key, sizeof( fd_pubkey_t ) ) ? pubkey : owner;

  /* TODO: use a proper lookup table */
  if ( !memcmp( lookup_pubkey, fd_solana_vote_program_id.key, sizeof( fd_pubkey_t ) ) ) {
    return fd_vote_program_execute;
  } else if ( !memcmp( lookup_pubkey, fd_solana_system_program_id.key, sizeof( fd_pubkey_t ) ) ) {
    return fd_system_program_execute;
  } else if ( !memcmp( lookup_pubkey, fd_solana_config_program_id.key, sizeof( fd_pubkey_t ) ) ) {
    return fd_config_program_execute;
  } else if ( !memcmp( lookup_pubkey, fd_solana_stake_program_id.key, sizeof( fd_pubkey_t ) ) ) {
    return fd_stake_program_execute;
  } else if ( !memcmp( lookup_pubkey, fd_solana_compute_budget_program_id.key, sizeof( fd_pubkey_t ) ) ) {
    return fd_compute_budget_program_execute;
  } else if( !memcmp( lookup_pubkey, fd_solana_address_lookup_table_program_id.key, sizeof( fd_pubkey_t ) ) ) {
    return fd_address_lookup_table_program_execute;
  } else if( !memcmp( lookup_pubkey, fd_solana_zk_elgamal_proof_program_id.key, sizeof( fd_pubkey_t ) ) ) {
    return fd_executor_zk_elgamal_proof_program_execute;
  } else if( !memcmp( lookup_pubkey, fd_solana_bpf_loader_deprecated_program_id.key, sizeof( fd_pubkey_t ))) {
    return fd_bpf_loader_program_execute;
  } else if( !memcmp( lookup_pubkey, fd_solana_bpf_loader_program_id.key, sizeof(fd_pubkey_t) ) ) {
    return fd_bpf_loader_program_execute;
  } else if( !memcmp( lookup_pubkey, fd_solana_bpf_loader_upgradeable_program_id.key, sizeof(fd_pubkey_t) ) ) {
    return fd_bpf_loader_program_execute;
  } else {
    return NULL;
  }
}

/* Returns 1 if the sysvar instruction is used, 0 otherwise */
uint
fd_executor_txn_uses_sysvar_instructions( fd_exec_txn_ctx_t const * txn_ctx ) {
  for( ulong i = 0; i < txn_ctx->accounts_cnt; i++ ) {
    if( FD_UNLIKELY( memcmp( txn_ctx->accounts[i].key, fd_sysvar_instructions_id.key, sizeof(fd_pubkey_t) ) == 0 ) ) {
      return 1;
    }
  }

  return 0;
}

int
is_invoked_account( fd_txn_t const * txn_descriptor, uchar idx ) {
  for ( uchar i = 0; i < txn_descriptor->instr_cnt; i++ ) {
    fd_txn_instr_t const * instr = &txn_descriptor->instr[i];
    if ( instr->program_id == idx ) return 1;
  }
  return 0;
}

int
is_passed_to_program_account( fd_txn_t const * txn_descriptor, fd_rawtxn_b_t const * raw_ptr, uchar idx ) {
  for ( uchar i = 0; i < txn_descriptor->instr_cnt; i++ ) {
    fd_txn_instr_t const * instr = &txn_descriptor->instr[i];
    uchar const * instr_accs = ((uchar const *)raw_ptr->raw + instr->acct_off );
    for ( uchar j = 0; j < instr->acct_cnt; j++ ) {
      if ( instr_accs[j] == idx ) return 1;
    }
  }
  return 0;
}

int
is_non_loader_program_key( fd_txn_t const * txn_descriptor, fd_rawtxn_b_t const * raw_ptr, uchar idx ) {
  return !is_invoked_account( txn_descriptor, idx ) || is_passed_to_program_account( txn_descriptor, raw_ptr, idx );
}

int
fd_executor_is_system_nonce_account( fd_borrowed_account_t * account ) {
FD_SCRATCH_SCOPE_BEGIN {
  if ( memcmp( account->const_meta->info.owner, fd_solana_system_program_id.uc, sizeof(fd_pubkey_t) ) == 0 ) {
    if ( account->const_meta->dlen == 0 ) {
      return 0;
    } else if ( account->const_meta->dlen == 80 ) { // TODO: nonce size macro
      fd_bincode_decode_ctx_t decode = { .data = account->const_data,
                                         .dataend = account->const_data + account->const_meta->dlen,
                                         .valloc = fd_scratch_virtual() };
      fd_nonce_state_versions_t nonce_versions;
      if (fd_nonce_state_versions_decode( &nonce_versions, &decode ) != 0 ) {
        return -1;
      }
      fd_nonce_state_t * state;;
      if ( fd_nonce_state_versions_is_current( &nonce_versions ) ) {
        state = &nonce_versions.inner.current;
      } else {
        state = &nonce_versions.inner.legacy;
      }

      if ( fd_nonce_state_is_initialized( state ) ) {
        return 1;
      }
    }
  }

  return -1;
} FD_SCRATCH_SCOPE_END;
}

int
check_rent_transition( fd_borrowed_account_t * account, fd_rent_t const * rent, ulong fee ) {
  ulong min_balance   = fd_rent_exempt_minimum_balance2( rent, account->const_meta->dlen );
  ulong pre_lamports  = account->const_meta->info.lamports;
  uchar pre_is_exempt = pre_lamports >= min_balance;

  ulong post_lamports  = pre_lamports - fee;
  uchar post_is_exempt = post_lamports >= min_balance;

  if ( post_lamports == 0 || post_is_exempt ) {
    return 1;
  }

  if ( pre_lamports == 0 || pre_is_exempt ) {
    return 0;
  }

  return post_lamports <= pre_lamports;
}

/* https://github.com/anza-xyz/agave/blob/v2.0.2/svm/src/account_loader.rs#L103 */
int
fd_validate_fee_payer( fd_borrowed_account_t * account, fd_rent_t const * rent, ulong fee ) {
  if( FD_UNLIKELY( account->const_meta->info.lamports==0UL ) ) {
    return FD_RUNTIME_TXN_ERR_ACCOUNT_NOT_FOUND;
  }

  ulong min_balance = 0UL;

  int is_nonce = fd_executor_is_system_nonce_account( account );
  if ( FD_UNLIKELY( is_nonce<0 ) ) {
    return FD_RUNTIME_TXN_ERR_INVALID_ACCOUNT_FOR_FEE;
  }

  if( is_nonce ) {
    min_balance = fd_rent_exempt_minimum_balance2( rent, 80 );
  }

  ulong out = ULONG_MAX;
  int cf = fd_ulong_checked_sub( account->const_meta->info.lamports, min_balance, &out);
  if( FD_UNLIKELY( cf!=FD_EXECUTOR_INSTR_SUCCESS ) ) {
    return FD_RUNTIME_TXN_ERR_INSUFFICIENT_FUNDS_FOR_FEE;
  }

  cf = fd_ulong_checked_sub( out, fee, &out );
  if( FD_UNLIKELY( cf!=FD_EXECUTOR_INSTR_SUCCESS ) ) {
    return FD_RUNTIME_TXN_ERR_INSUFFICIENT_FUNDS_FOR_FEE;
  }

  if( FD_UNLIKELY( account->const_meta->info.lamports<fee ) ) {
    return FD_RUNTIME_TXN_ERR_INSUFFICIENT_FUNDS_FOR_FEE;
  } else if( FD_UNLIKELY( memcmp( account->pubkey->key, fd_sysvar_incinerator_id.key, sizeof(fd_pubkey_t) ) != 0 &&
                          !check_rent_transition( account, rent, fee ) ) ) {
    return FD_RUNTIME_TXN_ERR_INSUFFICIENT_FUNDS_FOR_RENT;
  }

  return FD_RUNTIME_EXECUTE_SUCCESS;
}

static int
status_check_tower( ulong slot, void * _ctx ) {
  fd_exec_slot_ctx_t * ctx = (fd_exec_slot_ctx_t *)_ctx;
  if( slot==ctx->slot_bank.slot ) {
    return 1;
  }

  if( fd_txncache_is_rooted_slot( ctx->status_cache, slot ) ) {
    return 1;
  }

  if( fd_sysvar_slot_history_find_slot( ctx->slot_history, slot ) == FD_SLOT_HISTORY_SLOT_FOUND ) {
    return 1;
  }

  return 0;
}

int
fd_executor_check_status_cache( fd_exec_txn_ctx_t * txn_ctx ) {
  
  if( FD_UNLIKELY( !txn_ctx->slot_ctx->status_cache ) ) {
    return FD_RUNTIME_EXECUTE_SUCCESS;
  }

  fd_hash_t * blockhash = (fd_hash_t *)((uchar *)txn_ctx->_txn_raw->raw + txn_ctx->txn_descriptor->recent_blockhash_off);

  fd_txncache_query_t curr_query;
  curr_query.blockhash = blockhash->uc;
  fd_blake3_t b3[1];
  
  fd_blake3_init( b3 );
  fd_blake3_append( b3, ((uchar *)txn_ctx->_txn_raw->raw + txn_ctx->txn_descriptor->message_off),(ulong)( txn_ctx->_txn_raw->txn_sz - txn_ctx->txn_descriptor->message_off ) );
  fd_blake3_fini( b3, &txn_ctx->blake_txn_msg_hash );
  curr_query.txnhash = txn_ctx->blake_txn_msg_hash.uc;

  // TODO: figure out if it is faster to batch query properly and loop all txns again
  int err;
  fd_txncache_query_batch( txn_ctx->slot_ctx->status_cache, &curr_query, 1UL, txn_ctx->slot_ctx, status_check_tower, &err );
  return err;
}

/* https://github.com/anza-xyz/agave/blob/16de8b75ebcd57022409b422de557dd37b1de8db/sdk/src/transaction/sanitized.rs#L263-L275 */
int
fd_executor_verify_precompiles( fd_exec_txn_ctx_t * txn_ctx ) {
  ushort                 instr_cnt = txn_ctx->txn_descriptor->instr_cnt;
  fd_acct_addr_t const * tx_accs   = fd_txn_get_acct_addrs( txn_ctx->txn_descriptor, txn_ctx->_txn_raw->raw );
  for( ushort i=0; i<instr_cnt; i++ ) {
    fd_txn_instr_t  const * instr      = &txn_ctx->txn_descriptor->instr[i];
    fd_acct_addr_t  const * program_id = tx_accs + instr->program_id;
    if( !memcmp( program_id, &fd_solana_ed25519_sig_verify_program_id, sizeof(fd_pubkey_t) ) ) {
      if( FD_UNLIKELY( fd_precompile_ed25519_verify( txn_ctx, instr ) ) ) {
        return FD_RUNTIME_TXN_ERR_INVALID_ACCOUNT_INDEX;
      }
    } else if( !memcmp( program_id, &fd_solana_keccak_secp_256k_program_id, sizeof(fd_pubkey_t) )) {
      if( FD_UNLIKELY( fd_precompile_secp256k1_verify( txn_ctx, instr ) ) ) {
        return FD_RUNTIME_TXN_ERR_INVALID_ACCOUNT_INDEX;
      }
    }
  }
  return FD_RUNTIME_EXECUTE_SUCCESS;
}

/* https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L458-L475 */
static int
accumulate_and_check_loaded_account_data_size( ulong   acc_size,
                                               ulong   requested_loaded_accounts_data_size,
                                               ulong * accumulated_account_size ) {
  *accumulated_account_size = fd_ulong_sat_add( *accumulated_account_size, acc_size );
  if( FD_UNLIKELY( *accumulated_account_size>requested_loaded_accounts_data_size ) ) {
    return FD_RUNTIME_TXN_ERR_MAX_LOADED_ACCOUNTS_DATA_SIZE_EXCEEDED;
  }
  return FD_RUNTIME_EXECUTE_SUCCESS;
}

/* https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L261-L442 */
int
fd_executor_check_txn_program_accounts_and_data_sz( fd_exec_txn_ctx_t * txn_ctx ) {

  ulong requested_loaded_accounts_data_size = 0UL;

  if( FD_UNLIKELY( txn_ctx->loaded_accounts_data_size_limit==0UL ) ) {
    return FD_RUNTIME_TXN_ERR_INVALID_LOADED_ACCOUNTS_DATA_SIZE_LIMIT;
  }
  requested_loaded_accounts_data_size = txn_ctx->loaded_accounts_data_size_limit;

  ulong accumulated_account_size = 0UL;

  // https://github.com/anza-xyz/agave/blob/eff8961dcf7880aa7e438463203611a8a0088554/svm/src/account_loader.rs#L315-L335
  for( ulong i=0UL; i<txn_ctx->accounts_cnt; i++ ) {
    // Check for max loaded acct size
    fd_borrowed_account_t * acct = NULL;
    int err        = fd_txn_borrowed_account_view_idx( txn_ctx, (uchar)i, &acct );
    ulong acc_size = err==FD_ACC_MGR_SUCCESS ? acct->const_meta->dlen : 0UL; 
  
    err = accumulate_and_check_loaded_account_data_size( acc_size, requested_loaded_accounts_data_size, &accumulated_account_size );
    if( FD_UNLIKELY( err!=FD_RUNTIME_EXECUTE_SUCCESS ) ) {
      return err;
    }
  }

  // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L377-L433
  ushort instr_cnt = txn_ctx->txn_descriptor->instr_cnt;
  for( ushort i=0; i<instr_cnt; i++ ) {
    fd_txn_instr_t const * instr = &txn_ctx->txn_descriptor->instr[i];

    // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L384-L386
    fd_borrowed_account_t * program_account = NULL;
    int err = fd_txn_borrowed_account_view_idx( txn_ctx, instr->program_id, &program_account );
    if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
      return FD_RUNTIME_TXN_ERR_PROGRAM_ACCOUNT_NOT_FOUND;
    }

    // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L387-L389
    if( FD_UNLIKELY( !memcmp( program_account->pubkey->key, fd_solana_native_loader_id.key, sizeof(fd_pubkey_t) ) ) ) {
      continue;
    }

    // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L397-L400
    if( FD_UNLIKELY( !fd_account_is_executable( program_account->const_meta ) ) ) {
      return FD_RUNTIME_TXN_ERR_INVALID_PROGRAM_FOR_EXECUTION;
    }

    // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L402-L405
    // No FD_UNLIKELY or FD_LIKELY since it holds true for all native program invocations
    if( !memcmp( program_account->const_meta->info.owner, fd_solana_native_loader_id.key, sizeof(fd_pubkey_t) ) ) {
      continue;
    }

    /* https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L406-L410
       Side note: I don't think there's a single condition where this statement evaluates to false since builtins_start_index
       seems to be tied to accounts.len(), so I'll skip this check. Moving on... */
    
    // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L412-L429
    FD_BORROWED_ACCOUNT_DECL( owner_account );
    err = fd_acc_mgr_view( txn_ctx->slot_ctx->acc_mgr, txn_ctx->slot_ctx->funk_txn, (fd_pubkey_t *) program_account->const_meta->info.owner, owner_account );
    if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
      return FD_RUNTIME_TXN_ERR_PROGRAM_ACCOUNT_NOT_FOUND;
    }

    // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L413-L418
    if( FD_UNLIKELY( memcmp( owner_account->const_meta->info.owner, fd_solana_native_loader_id.key, sizeof(fd_pubkey_t) ) 
                  || !fd_account_is_executable( owner_account->const_meta ) ) ) {
      return FD_RUNTIME_TXN_ERR_INVALID_PROGRAM_FOR_EXECUTION;
    }

    // https://github.com/anza-xyz/agave/blob/ae18213c19ea5335dfc75e6b6116def0f0910aff/svm/src/account_loader.rs#L419-L424
    // TODO: Agave's implementation seems to double-count if the owner is part of the account keys...
    err = accumulate_and_check_loaded_account_data_size( owner_account->const_meta->dlen, requested_loaded_accounts_data_size, &accumulated_account_size );
    if( FD_UNLIKELY( err!=FD_RUNTIME_EXECUTE_SUCCESS ) ) {
      return err;
    }
  }

  return FD_RUNTIME_EXECUTE_SUCCESS;
}

// https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/transaction_processor.rs#L523
// This function combines the logic of load_program_accounts and load_program_with_pubkey
int
fd_executor_check_replenish_program_cache( fd_exec_txn_ctx_t * txn_ctx ) {
  int err = FD_RUNTIME_EXECUTE_SUCCESS;

  for( ulong i=0UL; i<txn_ctx->accounts_cnt; i++ ) {
    int hit_max_limit = 0;
    fd_borrowed_account_t * account = NULL;
    err                             = fd_txn_borrowed_account_view_idx( txn_ctx, (uchar)i, &account );
    // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L135
    if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
      continue;
    }

    // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/transaction_processor.rs#L254
    // Agave checks that the account owner is in PROGRAM_OWNERS (bpf_loader_upgradeable, bpf_loader, bpf_loader_deprecated, loader_v4)
    // After that filtering, those accounts are used in replenish_program_cache to update the program cache
    // Since we don't have a program cache, we only care about the checks that will cause sanitization errors

    // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L72
    if( !memcmp( account->const_meta->info.owner, fd_solana_bpf_loader_v4_program_id.uc, sizeof(fd_pubkey_t) ) ) {
      // ProgramAccountLoadResult::ProgramOfLoaderV4
      continue;
    }
    // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L86
    if( !memcmp( account->const_meta->info.owner, fd_solana_bpf_loader_deprecated_program_id.uc, sizeof(fd_pubkey_t) ) ) {
      // ProgramAccountLoadResult::ProgramOfLoaderV1
      // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L139
      continue;
    }
    // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L90
    if( !memcmp( account->const_meta->info.owner, fd_solana_bpf_loader_program_id.uc, sizeof(fd_pubkey_t) ) ) {
      // ProgramAccountLoadResult::ProgramOfLoaderV2
      // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L150
      continue;
    }
    // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L94
    fd_bpf_upgradeable_loader_state_t program_loader_state[1] = {0};
    err = 0;

    // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L94
    if( !memcmp( account->const_meta->info.owner, fd_solana_bpf_loader_upgradeable_program_id.uc, sizeof(fd_pubkey_t) ) ) {
      if( read_bpf_upgradeable_loader_state_for_program( txn_ctx, (uchar) i, program_loader_state, &err ) && fd_bpf_upgradeable_loader_state_is_program( program_loader_state ) ) {        // ProgramAccountLoadResult::ProgramOfLoaderV3
        fd_bincode_decode_ctx_t ctx = {
          .data    = (uchar *)account->const_meta + account->const_meta->hlen,
          .dataend = (char *) ctx.data + account->const_meta->dlen,
          .valloc  = fd_scratch_virtual(),
        };

        fd_bpf_upgradeable_loader_state_t loader_state[1];
        // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L99
        if( FD_LIKELY( !fd_bpf_upgradeable_loader_state_decode( loader_state, &ctx ) ) ) {
          fd_pubkey_t * programdata_pubkey = (fd_pubkey_t *)&loader_state->inner.program.programdata_address;

          fd_borrowed_account_t * programdata_account = NULL;
          err = fd_txn_borrowed_account_executable_view( txn_ctx, programdata_pubkey, &programdata_account );

          // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L99
          if ( err!=FD_ACC_MGR_SUCCESS ) {
            continue;
          };

          ulong acc_size = programdata_account->const_meta->dlen;
          // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/program_loader.rs#L164
          if( acc_size <= PROGRAMDATA_METADATA_SIZE  ) {
            // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/transaction_processor.rs#L601
            hit_max_limit = 1;
          }
        }
      }
    }

    if( hit_max_limit ) {
      // https://github.com/anza-xyz/agave/blob/df892c42418047ade3365c1b3ddcf6c45f95d1f1/svm/src/transaction_processor.rs#L271
      return FD_RUNTIME_TXN_ERR_PROGRAM_CACHE_HIT_MAX_LIMIT;
    }
  }

  return FD_RUNTIME_EXECUTE_SUCCESS;
}

int
fd_executor_collect_fees( fd_exec_txn_ctx_t * txn_ctx ) {

  fd_borrowed_account_t * rec = NULL;
  int err = fd_txn_borrowed_account_modify_idx( txn_ctx, 0, 0UL, &rec );
  if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
    return FD_RUNTIME_TXN_ERR_ACCOUNT_NOT_FOUND;
  }

  ulong execution_fee = 0UL;
  ulong priority_fee  = 0UL;

  fd_runtime_calculate_fee( txn_ctx, txn_ctx->txn_descriptor, txn_ctx->_txn_raw, &execution_fee, &priority_fee );

  fd_epoch_bank_t * epoch_bank = fd_exec_epoch_ctx_epoch_bank( txn_ctx->slot_ctx->epoch_ctx );
  ulong             total_fee  = 0UL;

  // https://github.com/anza-xyz/agave/blob/2e6ca8c1f62db62c1db7f19c9962d4db43d0d550/sdk/src/fee.rs#L54
  if ( FD_FEATURE_ACTIVE( txn_ctx->slot_ctx, remove_rounding_in_fee_calculation ) ) {
    total_fee  = fd_ulong_sat_add( execution_fee, priority_fee );
  } else {
    total_fee  = (ulong)round((double)fd_ulong_sat_add( execution_fee, priority_fee ));
  }

  err = fd_validate_fee_payer( rec, &epoch_bank->rent, total_fee );
  if( FD_UNLIKELY( err ) ) {
    return err;
  }

  /* At this point, the fee payer has been validated and the fee has been
     calculated. This means that the fee can be safely subtracted from the
     fee payer's borrowed account. However, the starting lamports of the 
     account must be updated as well. Each instruction must have the net 
     same (balanced) amount of lamports. This is done by comparing the 
     borrowed accounts starting lamports and comparing it to the sum of 
     the ending lamports. Therefore, we need to update the starting lamports
     specifically for the fee payer. 
     
     This is especially important in the case where the transaction fails. This
     is because we need to roll back the account to the balance AFTER the fee
     is paid. It is also possible for the accounts data and owner to change.
     This means that the entire state of the borrowed account must be rolled
     back to this point. */

  rec->meta->info.lamports -= total_fee;
  rec->starting_lamports    = rec->meta->info.lamports;

  /* Update the fee payer's rent epoch to ULONG_MAX if it is rent exempt. */
  if( FD_LIKELY( rec->const_meta->info.lamports>=fd_rent_exempt_minimum_balance2( &epoch_bank->rent, rec->const_meta->dlen ) ) ) {
    rec->meta->info.rent_epoch = ULONG_MAX;
  }

  txn_ctx->execution_fee = execution_fee;
  txn_ctx->priority_fee  = priority_fee;
  
  return FD_RUNTIME_EXECUTE_SUCCESS;
}

int
fd_executor_setup_accessed_accounts_for_txn( fd_exec_txn_ctx_t * txn_ctx ) {

  fd_pubkey_t * tx_accs   = (fd_pubkey_t *)((uchar *)txn_ctx->_txn_raw->raw + txn_ctx->txn_descriptor->acct_addr_off);

  // Set up accounts in the transaction body and perform checks
  for( ulong i = 0; i < txn_ctx->txn_descriptor->acct_addr_cnt; i++ ) {
    txn_ctx->accounts[i] = tx_accs[i];
  }

  txn_ctx->accounts_cnt += (uchar) txn_ctx->txn_descriptor->acct_addr_cnt;

  if( txn_ctx->txn_descriptor->transaction_version == FD_TXN_V0 ) {
    /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/runtime/src/bank/address_lookup_table.rs#L44-L48 */
    fd_slot_hashes_t const * slot_hashes = fd_sysvar_cache_slot_hashes( txn_ctx->slot_ctx->sysvar_cache );
    if( FD_UNLIKELY( !slot_hashes ) ) {
      return FD_RUNTIME_TXN_ERR_ACCOUNT_NOT_FOUND;
    }

    fd_pubkey_t readonly_lut_accs[128];
    ulong readonly_lut_accs_cnt = 0;
    FD_SCRATCH_SCOPE_BEGIN {
      // Set up accounts in the account look up tables.
      fd_txn_acct_addr_lut_t const * addr_luts = fd_txn_get_address_tables_const( txn_ctx->txn_descriptor );
      for( ulong i = 0; i < txn_ctx->txn_descriptor->addr_table_lookup_cnt; i++ ) {
        fd_txn_acct_addr_lut_t const * addr_lut = &addr_luts[i];
        fd_pubkey_t const * addr_lut_acc = (fd_pubkey_t *)((uchar *)txn_ctx->_txn_raw->raw + addr_lut->addr_off);

        /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/accounts-db/src/accounts.rs#L90-L94 */
        FD_BORROWED_ACCOUNT_DECL(addr_lut_rec);
        int err = fd_acc_mgr_view(txn_ctx->slot_ctx->acc_mgr, txn_ctx->slot_ctx->funk_txn, (fd_pubkey_t *) addr_lut_acc, addr_lut_rec);
        if( FD_UNLIKELY( err != FD_ACC_MGR_SUCCESS ) ) {
          return FD_RUNTIME_TXN_ERR_ADDRESS_LOOKUP_TABLE_NOT_FOUND;
        }

        /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/accounts-db/src/accounts.rs#L96-L114 */
        if( FD_UNLIKELY( memcmp( addr_lut_rec->const_meta->info.owner, fd_solana_address_lookup_table_program_id.key, sizeof(fd_pubkey_t) ) ) ) {
          return FD_RUNTIME_TXN_ERR_INVALID_ADDRESS_LOOKUP_TABLE_OWNER;
        }

        /* Realistically impossible case, but need to make sure we don't cause an OOB data access
           https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L205-L209 */
        if( FD_UNLIKELY( addr_lut_rec->const_meta->dlen < FD_LOOKUP_TABLE_META_SIZE ) ) {
          return FD_RUNTIME_TXN_ERR_INVALID_ADDRESS_LOOKUP_TABLE_DATA;
        }

        /* https://github.com/anza-xyz/agave/blob/574bae8fefc0ed256b55340b9d87b7689bcdf222/accounts-db/src/accounts.rs#L141-L142 */
        fd_address_lookup_table_state_t addr_lookup_table_state;
        fd_bincode_decode_ctx_t decode_ctx = {
          .data = addr_lut_rec->const_data,
          .dataend = &addr_lut_rec->const_data[FD_LOOKUP_TABLE_META_SIZE],
          .valloc  = fd_scratch_virtual(),
        };

        /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L197-L214 */
        if( FD_UNLIKELY( fd_address_lookup_table_state_decode( &addr_lookup_table_state, &decode_ctx ) ) ) {
          return FD_RUNTIME_TXN_ERR_INVALID_ADDRESS_LOOKUP_TABLE_DATA;
        }

        /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L200-L203 */
        if( FD_UNLIKELY( addr_lookup_table_state.discriminant != fd_address_lookup_table_state_enum_lookup_table ) ) {
          return FD_RUNTIME_TXN_ERR_INVALID_ADDRESS_LOOKUP_TABLE_DATA;
        }

        /* Again probably an impossible case, but the ALUT data needs to be 32-byte aligned
           https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L210-L214 */
        if( FD_UNLIKELY( ( addr_lut_rec->const_meta->dlen - FD_LOOKUP_TABLE_META_SIZE ) & 0x1fUL ) ) {
          return FD_RUNTIME_TXN_ERR_INVALID_ADDRESS_LOOKUP_TABLE_DATA;
        }

        /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/accounts-db/src/accounts.rs#L101-L112 */
        fd_pubkey_t * lookup_addrs = (fd_pubkey_t *)&addr_lut_rec->const_data[FD_LOOKUP_TABLE_META_SIZE];
        ulong lookup_addrs_cnt = ( addr_lut_rec->const_meta->dlen - FD_LOOKUP_TABLE_META_SIZE ) >> 5UL; // = (dlen - 56) / 32

        /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L175-L176 */
        ulong active_addresses_len;
        err = fd_get_active_addresses_len( &addr_lookup_table_state.inner.lookup_table, 
                                           txn_ctx->slot_ctx->slot_bank.slot,
                                           slot_hashes->hashes,
                                           lookup_addrs_cnt,
                                           &active_addresses_len );
        if( FD_UNLIKELY( err ) ) {
          return err;
        }

        /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L169-L182 */
        uchar * writable_lut_idxs = (uchar *)txn_ctx->_txn_raw->raw + addr_lut->writable_off;
        for( ulong j = 0; j < addr_lut->writable_cnt; j++ ) {
          /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L177-L181 */
          if( writable_lut_idxs[j] >= active_addresses_len ) {
            return FD_RUNTIME_TXN_ERR_INVALID_ADDRESS_LOOKUP_TABLE_INDEX;
          }
          txn_ctx->accounts[txn_ctx->accounts_cnt++] = lookup_addrs[writable_lut_idxs[j]];
        }

        uchar * readonly_lut_idxs = (uchar *)txn_ctx->_txn_raw->raw + addr_lut->readonly_off;
        for( ulong j = 0; j < addr_lut->readonly_cnt; j++ ) {
          /* https://github.com/anza-xyz/agave/blob/368ea563c423b0a85cc317891187e15c9a321521/sdk/program/src/address_lookup_table/state.rs#L177-L181 */
          if( readonly_lut_idxs[j] >= active_addresses_len ) {
            return FD_RUNTIME_TXN_ERR_INVALID_ADDRESS_LOOKUP_TABLE_INDEX;
          }
          readonly_lut_accs[readonly_lut_accs_cnt++] = lookup_addrs[readonly_lut_idxs[j]];
        }
      }
    } FD_SCRATCH_SCOPE_END;

    fd_memcpy( &txn_ctx->accounts[txn_ctx->accounts_cnt], readonly_lut_accs, readonly_lut_accs_cnt * sizeof(fd_pubkey_t) );
    txn_ctx->accounts_cnt += readonly_lut_accs_cnt;
  }
  return FD_RUNTIME_EXECUTE_SUCCESS;
}

int
fd_should_set_exempt_rent_epoch_max( fd_rent_t const *       rent,
                                     fd_borrowed_account_t * rec ) {
  if( fd_pubkey_is_sysvar_id( rec->pubkey ) ) {
    return 0;
  }
  /* If an account does not exist, that means that the rent epoch should be
     set because all new accounts must be rent-exempt. */
  if( rec->const_meta->info.lamports && rec->const_meta->info.lamports<fd_rent_exempt_minimum_balance2( rent, rec->const_meta->dlen ) ) {
    return 0;
  }
  if( rec->const_meta->info.rent_epoch==ULONG_MAX ) {
    return 0;
  }

  return 1;
}

void
fd_txn_set_exempt_rent_epoch_max( fd_exec_txn_ctx_t * txn_ctx,
                                  void const *        addr ) {
  fd_borrowed_account_t * rec = NULL;
  int err = fd_txn_borrowed_account_view( txn_ctx, (fd_pubkey_t const *)addr, &rec);
  if( FD_UNLIKELY( err==FD_ACC_MGR_ERR_UNKNOWN_ACCOUNT ) ) {
    return;
  } else if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
    FD_LOG_ERR(( "fd_txn_borrowed_account_view err=%d", err ));
  }

  if( !fd_should_set_exempt_rent_epoch_max( &txn_ctx->epoch_ctx->epoch_bank.rent, rec ) ) {
    return;
  }

  err = fd_txn_borrowed_account_modify( txn_ctx, (fd_pubkey_t const *)addr, 0, &rec);
  if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
    FD_LOG_ERR(( "fd_txn_borrowed_account_modify err=%d", err ));
  }

  if( FD_LIKELY( rec->meta ) ) {
    rec->meta->info.rent_epoch = ULONG_MAX;
  }
}

int
fd_executor_collect_fee( fd_borrowed_account_t const * rec,
                         ulong                         fee ) {
  if( FD_UNLIKELY( !rec->meta->info.lamports ) ) {
    return FD_RUNTIME_TXN_ERR_ACCOUNT_NOT_FOUND;
  }

  if( FD_UNLIKELY( fee>rec->meta->info.lamports ) ) {
    // TODO: Not enough lamports to pay for this txn...
    //
    // (Should this be lamps + whatever is required to keep the payer rent exempt?)
    FD_LOG_WARNING(( "Not enough lamps" ));
    return FD_RUNTIME_TXN_ERR_INSUFFICIENT_FUNDS_FOR_FEE;
  }

  rec->meta->info.lamports = fd_ulong_sat_sub( rec->meta->info.lamports, fee );

  return 0;
}

static void
dump_sorted_features( const fd_features_t * features, fd_exec_test_feature_set_t * output_feature_set ) {
  /* NOTE: Caller must have a scratch frame prepared */
  uint64_t * unsorted_features = fd_scratch_alloc( alignof(uint64_t), FD_FEATURE_ID_CNT * sizeof(uint64_t) );
  ulong num_features = 0;
  for( const fd_feature_id_t * current_feature = fd_feature_iter_init(); !fd_feature_iter_done( current_feature ); current_feature = fd_feature_iter_next( current_feature ) ) {
    if (features->f[current_feature->index] != FD_FEATURE_DISABLED) {
      unsorted_features[num_features++] = (uint64_t) current_feature->id.ul[0];
    }
  }
  // Sort the features
  void * scratch = fd_scratch_alloc( sort_uint64_t_stable_scratch_align(), sort_uint64_t_stable_scratch_footprint(num_features) );
  uint64_t * sorted_features = sort_uint64_t_stable_fast( unsorted_features, num_features, scratch );

  // Set feature set in message
  output_feature_set->features_count = (pb_size_t) num_features;
  output_feature_set->features       = sorted_features;
}

static void
dump_account_state( fd_borrowed_account_t const * borrowed_account,
                      fd_exec_test_acct_state_t * output_account ) {
    // Address
    fd_memcpy(output_account->address, borrowed_account->pubkey, sizeof(fd_pubkey_t));

    // Lamports
    output_account->lamports = (uint64_t) borrowed_account->const_meta->info.lamports;

    // Data
    output_account->data = fd_scratch_alloc(alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE(borrowed_account->const_meta->dlen));
    output_account->data->size = (pb_size_t) borrowed_account->const_meta->dlen;
    fd_memcpy(output_account->data->bytes, borrowed_account->const_data, borrowed_account->const_meta->dlen);

    // Executable
    output_account->executable = (bool) borrowed_account->const_meta->info.executable;

    // Rent epoch
    output_account->rent_epoch = (uint64_t) borrowed_account->const_meta->info.rent_epoch;

    // Owner
    fd_memcpy(output_account->owner, borrowed_account->const_meta->info.owner, sizeof(fd_pubkey_t));

    // Seed address (not present)
    output_account->has_seed_addr = false;
}

void
fd_create_instr_context_protobuf_from_instructions( fd_exec_test_instr_context_t * instr_context,
                                                 fd_exec_txn_ctx_t const *txn_ctx,
                                                 fd_instr_info_t const *instr ) {
  /*
  NOTE: Calling this function requires the caller to have a scratch frame ready (see dump_instr_to_protobuf)
  */

  /* Prepare sysvar cache accounts */
  fd_pubkey_t const fd_relevant_sysvar_ids[] = {
    fd_sysvar_clock_id,
    fd_sysvar_epoch_schedule_id,
    fd_sysvar_epoch_rewards_id,
    fd_sysvar_fees_id,
    fd_sysvar_rent_id,
    fd_sysvar_slot_hashes_id,
    fd_sysvar_recent_block_hashes_id,
    fd_sysvar_stake_history_id,
    fd_sysvar_last_restart_slot_id,
    fd_sysvar_instructions_id,
  };
  const ulong num_sysvar_entries = (sizeof(fd_relevant_sysvar_ids) / sizeof(fd_pubkey_t));

  /* Program ID */
  fd_memcpy( instr_context->program_id, instr->program_id_pubkey.uc, sizeof(fd_pubkey_t) );

  /* Accounts */
  instr_context->accounts_count = (pb_size_t) txn_ctx->accounts_cnt;
  instr_context->accounts = fd_scratch_alloc(alignof(fd_exec_test_acct_state_t), (instr_context->accounts_count + num_sysvar_entries + txn_ctx->executable_cnt) * sizeof(fd_exec_test_acct_state_t));
  for( ulong i = 0; i < txn_ctx->accounts_cnt; i++ ) {
    // Copy account information over
    fd_borrowed_account_t const * borrowed_account = &txn_ctx->borrowed_accounts[i];
    fd_exec_test_acct_state_t * output_account = &instr_context->accounts[i];
    dump_account_state( borrowed_account, output_account );
  }

  /* Add sysvar cache variables */
  for( ulong i = 0; i < num_sysvar_entries; i++ ) {
    FD_BORROWED_ACCOUNT_DECL(borrowed_account);
    int ret = fd_acc_mgr_view( txn_ctx->acc_mgr, txn_ctx->funk_txn, &fd_relevant_sysvar_ids[i], borrowed_account );
    if( ret != FD_ACC_MGR_SUCCESS ) {
      continue;
    }
    // Make sure the account doesn't exist in the output accounts yet
    int account_exists = 0;
    for( ulong j = 0; j < txn_ctx->accounts_cnt; j++ ) {
      if ( 0 == memcmp( txn_ctx->accounts[j].key, fd_relevant_sysvar_ids[i].uc, sizeof(fd_pubkey_t) ) ) {
        account_exists = true;
        break;
      }
    }

    // Copy it into output
    if (!account_exists) {
      fd_exec_test_acct_state_t * output_account = &instr_context->accounts[instr_context->accounts_count++];
      dump_account_state( borrowed_account, output_account );
    }
  }

  /* Add executable accounts */
  for( ulong i = 0; i < txn_ctx->executable_cnt; i++ ) {
    FD_BORROWED_ACCOUNT_DECL(borrowed_account);
    int ret = fd_acc_mgr_view( txn_ctx->acc_mgr, txn_ctx->funk_txn, txn_ctx->executable_accounts[i].pubkey, borrowed_account );
    if( ret != FD_ACC_MGR_SUCCESS ) {
      continue;
    }
    // Make sure the account doesn't exist in the output accounts yet
    bool account_exists = false;
    for( ulong j = 0; j < instr_context->accounts_count; j++ ) {
      if( 0 == memcmp( instr_context->accounts[j].address, txn_ctx->executable_accounts[i].pubkey->uc, sizeof(fd_pubkey_t) ) ) {
        account_exists = true;
        break;
      }
    }
    // Copy it into output
    if( !account_exists ) {
      fd_exec_test_acct_state_t * output_account = &instr_context->accounts[instr_context->accounts_count++];
      dump_account_state( borrowed_account, output_account );
    }
  }

  /* Instruction Accounts */
  instr_context->instr_accounts_count = (pb_size_t) instr->acct_cnt;
  instr_context->instr_accounts = fd_scratch_alloc( alignof(fd_exec_test_instr_acct_t), instr_context->instr_accounts_count * sizeof(fd_exec_test_instr_acct_t) );
  for( ushort i = 0; i < instr->acct_cnt; i++ ) {
    fd_exec_test_instr_acct_t * output_instr_account = &instr_context->instr_accounts[i];

    uchar account_flag = instr->acct_flags[i];
    bool is_writable = account_flag & FD_INSTR_ACCT_FLAGS_IS_WRITABLE;
    bool is_signer = account_flag & FD_INSTR_ACCT_FLAGS_IS_SIGNER;

    output_instr_account->index = instr->acct_txn_idxs[i];
    output_instr_account->is_writable = is_writable;
    output_instr_account->is_signer = is_signer;
  }

  /* Data */
  instr_context->data = fd_scratch_alloc( alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE(instr->data_sz) );
  instr_context->data->size = (pb_size_t) instr->data_sz;
  fd_memcpy( instr_context->data->bytes, instr->data, instr->data_sz );

  /* Compute Units */
  instr_context->cu_avail = txn_ctx->compute_meter;

  /* Slot Context */
  instr_context->has_slot_context = true;

  /* Epoch Context */
  instr_context->has_epoch_context = true;
  instr_context->epoch_context.has_features = true;
  dump_sorted_features( &txn_ctx->epoch_ctx->features, &instr_context->epoch_context.features );
}

/*  This function dumps individual instructions from a ledger replay.

    The following arguments can be added when replaying ledger transactions:
      --dump-insn-to-pb <0/1>
        * If enabled, instructions will be dumped to the specified output directory
      --dump-proto-sig-filter <base_58_enc_sig>
        * If enabled, only instructions with the specified transaction signature will be dumped
        * Provided signature must be base58-encoded
        * Default behavior if signature filter is not provided is to dump EVERY instruction
      --dump-proto-output-dir <output_dir>
        * Each file represents a single instruction as a serialized InstrContext Protobuf message
        * File name format is "instr-<base58_enc_sig>-<instruction_idx>.bin", where instruction_idx is 1-indexed

    solana-conformance (https://github.com/firedancer-io/solana-conformance)
      * Allows decoding / debugging of instructions in an isolated environment
      * Allows execution result(s) comparison with Solana / Agave
      * See solana-conformance/README.md for functionality and use cases
*/
static void
dump_instr_to_protobuf( fd_exec_txn_ctx_t *txn_ctx,
                        fd_instr_info_t *instr,
                        ushort instruction_idx ) {


  FD_SCRATCH_SCOPE_BEGIN {
    // Get base58-encoded tx signature
    const fd_ed25519_sig_t * signatures = fd_txn_get_signatures( txn_ctx->txn_descriptor, txn_ctx->_txn_raw->raw );
    fd_ed25519_sig_t signature; fd_memcpy( signature, signatures[0], sizeof(fd_ed25519_sig_t) );
    char encoded_signature[FD_BASE58_ENCODED_64_SZ];
    ulong out_size;
    fd_base58_encode_64( signature, &out_size, encoded_signature );

    if (txn_ctx->capture_ctx->dump_proto_sig_filter) {
      ulong filter_strlen = (ulong) strlen(txn_ctx->capture_ctx->dump_proto_sig_filter);

      // Terminate early if the signature does not match
      if( memcmp( txn_ctx->capture_ctx->dump_proto_sig_filter, encoded_signature, filter_strlen < out_size ? filter_strlen : out_size ) ) {
        return;
      }
    }

    fd_exec_test_instr_context_t instr_context = FD_EXEC_TEST_INSTR_CONTEXT_INIT_DEFAULT;
    fd_create_instr_context_protobuf_from_instructions( &instr_context, txn_ctx, instr );

    /* Output to file */
    ulong out_buf_size = 100 * 1024 * 1024;
    uint8_t * out = fd_scratch_alloc( alignof(uchar) , out_buf_size );
    pb_ostream_t stream = pb_ostream_from_buffer( out, out_buf_size );
    if (pb_encode(&stream, FD_EXEC_TEST_INSTR_CONTEXT_FIELDS, &instr_context)) {
      char output_filepath[256]; fd_memset(output_filepath, 0, sizeof(output_filepath));
      char * position = fd_cstr_init(output_filepath);
      position = fd_cstr_append_cstr(position, txn_ctx->capture_ctx->dump_proto_output_dir);
      position = fd_cstr_append_cstr(position, "/instr-");
      position = fd_cstr_append_cstr(position, encoded_signature);
      position = fd_cstr_append_cstr(position, "-");
      position = fd_cstr_append_ushort_as_text(position, '0', 0, instruction_idx, 3); // Assume max 3 digits
      position = fd_cstr_append_cstr(position, ".bin");
      fd_cstr_fini(position);

      FILE * file = fopen(output_filepath, "wb");
      if( file ) {
        fwrite( out, 1, stream.bytes_written, file );
        fclose( file );
      }
    }
  } FD_SCRATCH_SCOPE_END;
}

int
fd_execute_instr( fd_exec_txn_ctx_t * txn_ctx,
                  fd_instr_info_t *   instr ) {
  FD_SCRATCH_SCOPE_BEGIN {
    ulong max_num_instructions = FD_FEATURE_ACTIVE( txn_ctx->slot_ctx, limit_max_instruction_trace_length ) ? FD_MAX_INSTRUCTION_TRACE_LENGTH : ULONG_MAX;
    if( txn_ctx->num_instructions >= max_num_instructions ) {
      return FD_EXECUTOR_INSTR_ERR_MAX_INSN_TRACE_LENS_EXCEEDED;
    }
    txn_ctx->num_instructions++;
    fd_pubkey_t const * txn_accs = txn_ctx->accounts;

    ulong starting_lamports_h = 0;
    ulong starting_lamports_l = 0;
    int err = fd_instr_info_sum_account_lamports( instr, &starting_lamports_h, &starting_lamports_l );
    if( err ) {
      return err;
    }
    instr->starting_lamports_h = starting_lamports_h;
    instr->starting_lamports_l = starting_lamports_l;

    fd_exec_instr_ctx_t * parent = NULL;
    if( txn_ctx->instr_stack_sz )
      parent = &txn_ctx->instr_stack[ txn_ctx->instr_stack_sz - 1 ];

    fd_exec_instr_ctx_t * ctx = &txn_ctx->instr_stack[ txn_ctx->instr_stack_sz++ ];
    *ctx = (fd_exec_instr_ctx_t) {
      .instr     = instr,
      .txn_ctx   = txn_ctx,
      .epoch_ctx = txn_ctx->epoch_ctx,
      .slot_ctx  = txn_ctx->slot_ctx,
      .valloc    = fd_scratch_virtual(),
      .acc_mgr   = txn_ctx->acc_mgr,
      .funk_txn  = txn_ctx->funk_txn,
      .parent    = parent,
      .index     = parent ? (parent->child_cnt++) : 0,
      .depth     = parent ? (parent->depth+1    ) : 0,
      .child_cnt = 0U,
    };

    /* Add the instruction to the trace */
    txn_ctx->instr_trace[ txn_ctx->instr_trace_length++ ] = (fd_exec_instr_trace_entry_t) {
      .instr_info = instr,
      .stack_height = txn_ctx->instr_stack_sz,
    };

    // defense in depth
    if( instr->program_id >= txn_ctx->txn_descriptor->acct_addr_cnt + txn_ctx->txn_descriptor->addr_table_adtl_cnt ) {
      FD_LOG_WARNING(( "INVALID PROGRAM ID, RUNTIME BUG!!!" ));
      int exec_result = FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS;
      txn_ctx->instr_stack_sz--;

      FD_LOG_WARNING(( "instruction executed unsuccessfully: error code %d", exec_result ));
      return exec_result;
    }

    fd_exec_instr_fn_t  native_prog_fn = fd_executor_lookup_native_program( &txn_ctx->borrowed_accounts[ instr->program_id ] );
    fd_pubkey_t const * program_id     = &txn_accs[ instr->program_id ];

    /* TODO: this is a hack because the programs should've been verified already
       if we reach this point that means the transaction was succesful. */
    if( !memcmp( program_id, fd_solana_ed25519_sig_verify_program_id.key, sizeof( fd_pubkey_t ) ) ) {
      txn_ctx->instr_stack_sz--;
      return 0;
    }
    if( !memcmp( program_id, fd_solana_keccak_secp_256k_program_id.key, sizeof( fd_pubkey_t ) ) ) {
      txn_ctx->instr_stack_sz--;
      return 0;
    }

    fd_exec_txn_ctx_reset_return_data( txn_ctx );
    int exec_result = FD_EXECUTOR_INSTR_SUCCESS;
    if( native_prog_fn != NULL ) {
      /* Log program invokation (internally caches program_id base58) */
      fd_log_collector_program_invoke( ctx );
      exec_result = native_prog_fn( ctx );
    } else {
      exec_result = FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID;
    }

    if( exec_result == FD_EXECUTOR_INSTR_SUCCESS ) {
      ulong ending_lamports_h = 0UL;
      ulong ending_lamports_l = 0UL;
      err = fd_instr_info_sum_account_lamports( instr, &ending_lamports_h, &ending_lamports_l );
      if( FD_UNLIKELY( err ) ) {
        txn_ctx->instr_stack_sz--;
        return err;
      }

      if( FD_UNLIKELY( ending_lamports_l != starting_lamports_l || ending_lamports_h != starting_lamports_h ) ) {
        exec_result = FD_EXECUTOR_INSTR_ERR_UNBALANCED_INSTR;
      }

      /* TODO where does Agave do this? */
      for( ulong j=0UL; j < txn_ctx->accounts_cnt; j++ ) {
        if( FD_UNLIKELY( txn_ctx->borrowed_accounts[j].refcnt_excl ) ) {
          FD_LOG_ERR(( "Txn %64J: Program %32J didn't release lock (%u) on %32J with %u refcnt", fd_txn_get_signatures( txn_ctx->txn_descriptor, txn_ctx->_txn_raw->raw )[0], instr->program_id_pubkey.uc, *(uint *)(instr->data), txn_ctx->borrowed_accounts[j].pubkey->uc, txn_ctx->borrowed_accounts[j].refcnt_excl ));
        }
      }

      /* Log success */
      fd_log_collector_program_success( ctx );
    } else {
      if( !txn_ctx->exec_err ) {
        FD_TXN_ERR_FOR_LOG_INSTR( txn_ctx, exec_result, txn_ctx->instr_err_idx );
      }

      if( !txn_ctx->failed_instr ) {
        txn_ctx->failed_instr = ctx;
        ctx->instr_err        = (uint)( -exec_result - 1 );
      }

      /* Log failure cases.
         We assume that the correct type of error is stored in ctx.
         Syscalls are expected to log when the error is generated, while
         native programs will be logged here.
         (This is because syscall errors often carry data with them.) */
      fd_log_collector_program_failure( ctx );
    }

#ifdef VLOG
  if ( FD_UNLIKELY( exec_result != FD_EXECUTOR_INSTR_SUCCESS ) ) {
    FD_LOG_WARNING(( "instruction executed unsuccessfully: error code %d, custom err: %d, program id: %32J", exec_result, txn_ctx->custom_err, instr->program_id_pubkey.uc ));
  } else {
    FD_LOG_WARNING(( "instruction executed successfully: error code %d, custom err: %d, program id: %32J", exec_result, txn_ctx->custom_err, instr->program_id_pubkey.uc ));
  }
#endif

    txn_ctx->instr_stack_sz--;

    /* TODO: sanity before/after checks: total lamports unchanged etc */
    return exec_result;
  } FD_SCRATCH_SCOPE_END;
}

void
fd_txn_reclaim_accounts( fd_exec_txn_ctx_t * txn_ctx ) {
  for( ulong i = 0; i < txn_ctx->accounts_cnt; i++ ) {
    fd_borrowed_account_t * acc_rec = &txn_ctx->borrowed_accounts[i];

    /* An account writable iff it is writable AND it is not being demoted.
        If this criteria is not met, the account should not be marked as touched
        via updating its most recent slot. */
    if( !fd_txn_account_is_writable_idx( txn_ctx, (int)i ) ) {
      continue;
    }

    acc_rec->meta->slot = txn_ctx->slot_ctx->slot_bank.slot;

    if( acc_rec->meta->info.lamports == 0 ) {
      acc_rec->meta->dlen = 0;
      memset( acc_rec->meta->info.owner, 0, sizeof(fd_pubkey_t) );
    }
  }
}

int
fd_executor_is_blockhash_valid_for_age( fd_block_hash_queue_t const * block_hash_queue,
                                        fd_hash_t const *             blockhash,
                                        ulong                         max_age ) {
  fd_hash_hash_age_pair_t_mapnode_t key;
  fd_memcpy( key.elem.key.uc, blockhash, sizeof(fd_hash_t) );

  fd_hash_hash_age_pair_t_mapnode_t * hash_age = fd_hash_hash_age_pair_t_map_find( block_hash_queue->ages_pool, block_hash_queue->ages_root, &key );
  if( hash_age==NULL ) {
    #ifdef VLOG
    FD_LOG_WARNING(( "txn with missing recent blockhash - blockhash: %32J", blockhash->uc ));
    #endif
    return 0;
  }
  ulong age = block_hash_queue->last_hash_index-hash_age->elem.val.hash_index;
#ifdef VLOG
  if( age>max_age ) {
    FD_LOG_WARNING(( "txn with old blockhash - age: %lu, blockhash: %32J", age, hash_age->elem.key.uc ));
  }
#endif
  return ( age<=max_age );
}

void
fd_executor_setup_borrowed_accounts_for_txn( fd_exec_txn_ctx_t * txn_ctx ) {
  ulong j = 0;
  for( ulong i = 0; i < txn_ctx->accounts_cnt; i++ ) {
    fd_pubkey_t * acc = &txn_ctx->accounts[i];

    fd_borrowed_account_t * borrowed_account = fd_borrowed_account_init( &txn_ctx->borrowed_accounts[i] );
    fd_acc_mgr_view( txn_ctx->acc_mgr, txn_ctx->funk_txn, acc, borrowed_account );
    memcpy(borrowed_account->pubkey->key, acc, sizeof(*acc));

    if( fd_txn_account_is_writable_idx( txn_ctx, (int)i ) ) {
      void * borrowed_account_data = fd_valloc_malloc( txn_ctx->valloc, 8UL, fd_borrowed_account_raw_size( borrowed_account ) );
      fd_borrowed_account_make_modifiable( borrowed_account, borrowed_account_data );
    }

    fd_account_meta_t const * meta = borrowed_account->const_meta ? borrowed_account->const_meta : borrowed_account->meta;
    if( meta == NULL ) {
      static const fd_account_meta_t sentinel = { .magic = FD_ACCOUNT_META_MAGIC };
      borrowed_account->const_meta        = &sentinel;
      borrowed_account->starting_lamports = 0UL;
      borrowed_account->starting_dlen     = 0UL;
      continue;
    }

    if( meta->info.executable ) {
      FD_BORROWED_ACCOUNT_DECL(owner_borrowed_account);
      int err = fd_acc_mgr_view( txn_ctx->acc_mgr, txn_ctx->funk_txn, (fd_pubkey_t *)meta->info.owner, owner_borrowed_account );
      if( FD_UNLIKELY( err ) ) {
        borrowed_account->starting_owner_dlen = 0;
      } else {
        borrowed_account->starting_owner_dlen = owner_borrowed_account->const_meta->dlen;
      }
    }

    if( FD_UNLIKELY( memcmp( meta->info.owner, fd_solana_bpf_loader_upgradeable_program_id.key, sizeof(fd_pubkey_t) ) == 0 ) ) {
      fd_bpf_upgradeable_loader_state_t program_loader_state = {0};
      int err = 0;
      if( FD_UNLIKELY( !read_bpf_upgradeable_loader_state_for_program( txn_ctx, (uchar) i, &program_loader_state, &err ) ) ) {
        continue;
      }

      if( !fd_bpf_upgradeable_loader_state_is_program( &program_loader_state ) ) {
        continue;
      }

      fd_pubkey_t * programdata_acc = &program_loader_state.inner.program.programdata_address;
      fd_borrowed_account_t * executable_account = fd_borrowed_account_init( &txn_ctx->executable_accounts[j] );
      fd_acc_mgr_view( txn_ctx->acc_mgr, txn_ctx->funk_txn, programdata_acc, executable_account);
      j++;
    }
  }
  txn_ctx->executable_cnt = j;
}

/* Stuff to be done before multithreading can begin */
int
fd_execute_txn_prepare_start( fd_exec_slot_ctx_t *  slot_ctx,
                              fd_exec_txn_ctx_t *   txn_ctx,
                              fd_txn_t const *      txn_descriptor,
                              fd_rawtxn_b_t const * txn_raw ) {
  /* Init txn ctx */
  fd_exec_txn_ctx_new( txn_ctx );
  fd_exec_txn_ctx_from_exec_slot_ctx( slot_ctx, txn_ctx );
  fd_exec_txn_ctx_setup( txn_ctx, txn_descriptor, txn_raw );

  /* Unroll accounts from aluts and place into correct spots */
  int res = fd_executor_setup_accessed_accounts_for_txn( txn_ctx );

  return res;
  /* TODO:FIXME: MOVE THIS PELASE */
}

int
fd_executor_txn_verify( fd_exec_txn_ctx_t * txn_ctx ) {
  FD_SCRATCH_SCOPE_BEGIN {
    fd_sha512_t * shas[ FD_TXN_ACTUAL_SIG_MAX ];
    for ( ulong i=0; i<FD_TXN_ACTUAL_SIG_MAX; i++ ) {
      fd_sha512_t * sha = fd_sha512_join( fd_sha512_new( fd_scratch_alloc( alignof( fd_sha512_t ), sizeof( fd_sha512_t ) ) ) );
      if( FD_UNLIKELY( !sha ) ) FD_LOG_ERR(( "fd_sha512_join failed" ));
      shas[i] = sha;
    }

    uchar  signature_cnt = txn_ctx->txn_descriptor->signature_cnt;
    ushort signature_off = txn_ctx->txn_descriptor->signature_off;
    ushort acct_addr_off = txn_ctx->txn_descriptor->acct_addr_off;
    ushort message_off   = txn_ctx->txn_descriptor->message_off;

    uchar const * signatures = (uchar *)txn_ctx->_txn_raw->raw + signature_off;
    uchar const * pubkeys = (uchar *)txn_ctx->_txn_raw->raw + acct_addr_off;
    uchar const * msg = (uchar *)txn_ctx->_txn_raw->raw + message_off;
    ulong msg_sz = (ulong)txn_ctx->_txn_raw->txn_sz - message_off;

    /* Verify signatures */
    int res = fd_ed25519_verify_batch_single_msg( msg, msg_sz, signatures, pubkeys, shas, signature_cnt );
    if( FD_UNLIKELY( res != FD_ED25519_SUCCESS ) ) {
      return -1;
    }

    return 0;
  } FD_SCRATCH_SCOPE_END;
}

int
fd_execute_txn_prepare_phase3( fd_exec_slot_ctx_t * slot_ctx,
                               fd_exec_txn_ctx_t *  txn_ctx,
                               fd_txn_p_t *         txn ) {
  /* TODO: These checks should be moved to phase2. */

  if (FD_FEATURE_ACTIVE( txn_ctx->slot_ctx, apply_cost_tracker_during_replay ) ) {
    ulong est_cost = fd_pack_compute_cost( TXN(txn), txn->payload, &txn->flags, NULL, NULL, NULL );
    if( slot_ctx->total_compute_units_requested + est_cost <= MAX_COMPUTE_UNITS_PER_BLOCK ) {
      slot_ctx->total_compute_units_requested += est_cost;
    } else {
      return FD_RUNTIME_TXN_ERR_WOULD_EXCEED_MAX_BLOCK_COST_LIMIT;
    }

    fd_pubkey_t * tx_accs   = txn_ctx->accounts;
    for( fd_txn_acct_iter_t ctrl = fd_txn_acct_iter_init( txn_ctx->txn_descriptor, FD_TXN_ACCT_CAT_WRITABLE & FD_TXN_ACCT_CAT_IMM );
         ctrl != fd_txn_acct_iter_end(); ctrl=fd_txn_acct_iter_next( ctrl ) ) {
      ulong i = fd_txn_acct_iter_idx( ctrl );
      fd_pubkey_t * acct = &tx_accs[i];
      if (!fd_txn_account_is_writable_idx( txn_ctx, (int)i )) {
        continue;
      }
      fd_account_compute_elem_t * elem = fd_account_compute_table_query( slot_ctx->account_compute_table, acct, NULL );
      if ( !elem ) {
        elem = fd_account_compute_table_insert( slot_ctx->account_compute_table, acct );
        elem->cu_consumed = 0;
      }

      if ( elem->cu_consumed + est_cost > MAX_COMPUTE_UNITS_PER_WRITE_LOCKED_ACCOUNT ) {
        return FD_RUNTIME_TXN_ERR_WOULD_EXCEED_MAX_ACCOUNT_COST_LIMIT;
      }

      elem->cu_consumed += est_cost;
    }
  }

  return 0;
}

int
fd_execute_txn_prepare_finish( fd_exec_txn_ctx_t * txn_ctx ) {

  /* Update rent exempt on writable accounts if feature activated. All
     writable accounts that don't exist, will already be marked as being
     rent exempt (rent_epoch==ULONG_MAX). */
  for( ulong i=0UL; i<txn_ctx->accounts_cnt; i++ ) {
    txn_ctx->unknown_accounts[ i ] = 0;
    txn_ctx->nonce_accounts  [ i ] = 0;
    if( fd_txn_is_writable( txn_ctx->txn_descriptor, (int)i ) ) {
      if( FD_LIKELY( i!=0UL ) ) {
        fd_txn_set_exempt_rent_epoch_max( txn_ctx, &txn_ctx->accounts[ i ] );
      }
      FD_BORROWED_ACCOUNT_DECL( writable_new );
      int err = fd_acc_mgr_view( txn_ctx->acc_mgr, txn_ctx->funk_txn, &txn_ctx->accounts[ i ], writable_new );
      if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
        txn_ctx->unknown_accounts[ i ] = 1;
      }
    }
  }

  return 0;
}

/* Stuff to be done after multithreading ends */
int
fd_execute_txn_finalize( fd_exec_txn_ctx_t * txn_ctx,
                         int exec_txn_err ) {
  if( exec_txn_err != 0 ) {
    for( ulong i = 0; i < txn_ctx->accounts_cnt; i++ ) {
      fd_borrowed_account_t * acc_rec = &txn_ctx->borrowed_accounts[i];
      void * acc_rec_data = fd_borrowed_account_destroy( acc_rec );
      if( acc_rec_data != NULL ) {
        fd_valloc_free( txn_ctx->valloc, acc_rec_data );
      }
    }

    // fd_funk_txn_cancel( slot_ctx->acc_mgr->funk, txn_ctx->funk_txn, 0 );
    return 0;
  }

  for( ulong i = 0; i < txn_ctx->accounts_cnt; i++ ) {
    if( !fd_txn_account_is_writable_idx( txn_ctx, (int)i ) ) {
      continue;
    }

    fd_borrowed_account_t * acc_rec = &txn_ctx->borrowed_accounts[i];

    if( txn_ctx->unknown_accounts[i] ) {
      memset( acc_rec->meta->hash, 0xFF, sizeof(fd_hash_t) );
      fd_txn_set_exempt_rent_epoch_max( txn_ctx, &txn_ctx->accounts[i] );
    }

    int ret = fd_acc_mgr_save_non_tpool( txn_ctx->acc_mgr, txn_ctx->funk_txn, acc_rec );
    if( ret != FD_ACC_MGR_SUCCESS ) {
      FD_LOG_ERR(( "failed to save edits to accounts" ));
      return -1;
    }

    void * borrow_account_data = fd_borrowed_account_destroy( acc_rec );
    if( borrow_account_data != NULL ) {
      fd_valloc_free( txn_ctx->valloc, borrow_account_data );
    }
  }

  return 0;
}

/* Creates a TxnContext Protobuf message from a provided txn_ctx.
   - The transaction is assumed to have just finished phase 1 of preparation
   - Caller of this function should have a scratch frame ready 
*/
static void
create_txn_context_protobuf_from_txn( fd_exec_test_txn_context_t * txn_context_msg,
                                               fd_exec_txn_ctx_t *          txn_ctx ) {
  fd_txn_t const * txn_descriptor = txn_ctx->txn_descriptor;
  uchar const * txn_payload = (uchar const *) txn_ctx->_txn_raw->raw;
  fd_exec_slot_ctx_t * slot_ctx = txn_ctx->slot_ctx;

  /* We don't want to store builtins in account shared data */
  fd_pubkey_t const loaded_builtins[] = {
    fd_solana_system_program_id,
    fd_solana_vote_program_id,
    fd_solana_stake_program_id,
    fd_solana_config_program_id,
    // fd_solana_zk_token_proof_program_id,
    fd_solana_bpf_loader_v4_program_id,
    fd_solana_address_lookup_table_program_id,
    fd_solana_bpf_loader_deprecated_program_id,
    fd_solana_bpf_loader_program_id,
    fd_solana_bpf_loader_upgradeable_program_id,
    fd_solana_compute_budget_program_id,
    fd_solana_keccak_secp_256k_program_id,
    fd_solana_ed25519_sig_verify_program_id,
    fd_solana_zk_elgamal_proof_program_id,
    fd_solana_ed25519_sig_verify_program_id,
    fd_solana_spl_native_mint_id,
  };
  const ulong num_loaded_builtins = (sizeof(loaded_builtins) / sizeof(fd_pubkey_t));

  /* Prepare sysvar cache accounts */
  fd_pubkey_t const fd_relevant_sysvar_ids[] = {
    fd_sysvar_clock_id,
    fd_sysvar_epoch_schedule_id,
    fd_sysvar_epoch_rewards_id,
    fd_sysvar_fees_id,
    fd_sysvar_rent_id,
    fd_sysvar_slot_hashes_id,
    fd_sysvar_recent_block_hashes_id,
    fd_sysvar_stake_history_id,
    fd_sysvar_last_restart_slot_id,
    fd_sysvar_instructions_id,
  };
  const ulong num_sysvar_entries = (sizeof(fd_relevant_sysvar_ids) / sizeof(fd_pubkey_t));

  /* Transaction Context -> tx */
  txn_context_msg->has_tx = true;
  fd_exec_test_sanitized_transaction_t * sanitized_transaction = &txn_context_msg->tx;

  /* Transaction Context -> tx -> message */
  sanitized_transaction->has_message = true;
  fd_exec_test_transaction_message_t * message = &sanitized_transaction->message;

  /* Transaction Context -> tx -> message -> is_legacy */
  message->is_legacy = txn_descriptor->transaction_version == FD_TXN_VLEGACY;

  /* Transaction Context -> tx -> message -> header */
  message->has_header = true;
  fd_exec_test_message_header_t * header = &message->header;

  /* Transaction Context -> tx -> message -> header -> num_required_signatures */
  header->num_required_signatures = txn_descriptor->signature_cnt;

  /* Transaction Context -> tx -> message -> header -> num_readonly_signed_accounts */
  header->num_readonly_signed_accounts = txn_descriptor->readonly_signed_cnt;

  /* Transaction Context -> tx -> message -> header -> num_readonly_unsigned_accounts */
  header->num_readonly_unsigned_accounts = txn_descriptor->readonly_unsigned_cnt;

  /* Transaction Context -> tx -> message -> account_keys */
  message->account_keys_count = txn_descriptor->acct_addr_cnt;
  message->account_keys = fd_scratch_alloc( alignof(pb_bytes_array_t *), PB_BYTES_ARRAY_T_ALLOCSIZE(txn_descriptor->acct_addr_cnt * sizeof(pb_bytes_array_t *)) );
  fd_acct_addr_t const * account_keys = fd_txn_get_acct_addrs( txn_descriptor, txn_payload );
  for( ulong i = 0; i < txn_descriptor->acct_addr_cnt; i++ ) {
    pb_bytes_array_t * account_key = fd_scratch_alloc( alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE(sizeof(fd_pubkey_t)) );
    account_key->size = sizeof(fd_pubkey_t);
    memcpy( account_key->bytes, &account_keys[i], sizeof(fd_pubkey_t) );
    message->account_keys[i] = account_key;
  }

  /* Transaction Context -> tx -> message -> account_shared_data 
     Contains:
      - Account data for regular accounts
      - Account data for LUT accounts
      - Account data for executable accounts
      - Account data for (almost) all sysvars
  */
  // Dump regular accounts first
  message->account_shared_data_count = 0;
  message->account_shared_data = fd_scratch_alloc( alignof(fd_exec_test_acct_state_t), 
                                                   (txn_ctx->accounts_cnt * 2 + txn_descriptor->addr_table_lookup_cnt + num_sysvar_entries) * sizeof(fd_exec_test_acct_state_t) );
  for( ulong i = 0; i < txn_ctx->accounts_cnt; ++i ) {
    FD_BORROWED_ACCOUNT_DECL(borrowed_account);
    int ret = fd_acc_mgr_view( slot_ctx->acc_mgr, slot_ctx->funk_txn, &txn_ctx->accounts[i], borrowed_account );
    if( FD_UNLIKELY(ret != FD_ACC_MGR_SUCCESS) ) {
      continue;
    }

    // Make sure account is not a builtin
    bool is_builtin = false;
    for( ulong j = 0; j < num_loaded_builtins; ++j ) {
      if( 0 == memcmp( &txn_ctx->accounts[i], &loaded_builtins[j], sizeof(fd_pubkey_t) ) ) {
        is_builtin = true;
        break;
      }
    }
    if( !is_builtin ) {
      dump_account_state( borrowed_account, &message->account_shared_data[message->account_shared_data_count++] );
    }
  }

  // For executable accounts, we need to set up dummy borrowed accounts by cluttering txn ctx state and resetting it after
  // TODO: Revisit this hacky approach
  fd_valloc_t orig_valloc = txn_ctx->valloc;
  txn_ctx->valloc = fd_scratch_virtual();
  txn_ctx->funk_txn = slot_ctx->funk_txn;
  fd_executor_setup_borrowed_accounts_for_txn( txn_ctx );

  // Dump executable accounts
  for( ulong i = 0; i < txn_ctx->executable_cnt; ++i ) {
    if( !txn_ctx->executable_accounts[i].const_meta ) {
      continue;
    }
    dump_account_state( &txn_ctx->executable_accounts[i], &message->account_shared_data[message->account_shared_data_count++] );
  }

  // Reset state
  txn_ctx->valloc = orig_valloc;
  txn_ctx->funk_txn = NULL;
  txn_ctx->executable_cnt = 0;

  // Dump LUT accounts
  fd_txn_acct_addr_lut_t const * address_lookup_tables = fd_txn_get_address_tables_const( txn_descriptor );
  for( ulong i = 0; i < txn_descriptor->addr_table_lookup_cnt; ++i ) {
    FD_BORROWED_ACCOUNT_DECL(borrowed_account);
    fd_pubkey_t * alut_key = (fd_pubkey_t *) (txn_payload + address_lookup_tables[i].addr_off);
    int ret = fd_acc_mgr_view( slot_ctx->acc_mgr, slot_ctx->funk_txn, alut_key, borrowed_account );
    if( FD_UNLIKELY(ret != FD_ACC_MGR_SUCCESS) ) {
      continue;
    }
    dump_account_state( borrowed_account, &message->account_shared_data[message->account_shared_data_count++] );
  }

  // Dump sysvars
  for( ulong i = 0; i < num_sysvar_entries; i++ ) {
    FD_BORROWED_ACCOUNT_DECL(borrowed_account);
    int ret = fd_acc_mgr_view( slot_ctx->acc_mgr, slot_ctx->funk_txn, &fd_relevant_sysvar_ids[i], borrowed_account );
    if( ret != FD_ACC_MGR_SUCCESS ) {
      continue;
    }

    // Make sure the account doesn't exist in the output accounts yet
    int account_exists = 0;
    for( ulong j = 0; j < txn_ctx->accounts_cnt; j++ ) {
      if ( 0 == memcmp( txn_ctx->accounts[j].key, fd_relevant_sysvar_ids[i].uc, sizeof(fd_pubkey_t) ) ) {
        account_exists = true;
        break;
      }
    }
    // Copy it into output
    if (!account_exists) {
      dump_account_state( borrowed_account, &message->account_shared_data[message->account_shared_data_count++] );
    }
  }

  /* Transaction Context -> tx -> message -> recent_blockhash */
  uchar const * recent_blockhash = fd_txn_get_recent_blockhash( txn_descriptor, txn_payload );
  message->recent_blockhash = fd_scratch_alloc( alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE(sizeof(fd_hash_t)) );
  message->recent_blockhash->size = sizeof(fd_hash_t);
  memcpy( message->recent_blockhash->bytes, recent_blockhash, sizeof(fd_hash_t) );

  /* Transaction Context -> tx -> message -> instructions */
  message->instructions_count = txn_descriptor->instr_cnt;
  message->instructions = fd_scratch_alloc( alignof(fd_exec_test_compiled_instruction_t), txn_descriptor->instr_cnt * sizeof(fd_exec_test_compiled_instruction_t) );
  for( ulong i = 0; i < txn_descriptor->instr_cnt; ++i ) {
    fd_txn_instr_t instr = txn_descriptor->instr[i];
    fd_exec_test_compiled_instruction_t * compiled_instruction = &message->instructions[i];

    // compiled instruction -> program_id_index
    compiled_instruction->program_id_index = instr.program_id;

    // compiled instruction -> accounts
    compiled_instruction->accounts_count = instr.acct_cnt;
    compiled_instruction->accounts = fd_scratch_alloc( alignof(uint32_t), instr.acct_cnt * sizeof(uint32_t) );
    uchar const * instr_accounts = fd_txn_get_instr_accts( &instr, txn_payload );
    for( ulong j = 0; j < instr.acct_cnt; ++j ) {
      uchar instr_acct_index = instr_accounts[j];
      compiled_instruction->accounts[j] = instr_acct_index;
    }

    // compiled instruction -> data
    uchar const * instr_data = fd_txn_get_instr_data( &instr, txn_payload );
    compiled_instruction->data = fd_scratch_alloc( alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE(instr.data_sz) );
    compiled_instruction->data->size = instr.data_sz;
    memcpy( compiled_instruction->data->bytes, instr_data, instr.data_sz );
  }

  /* ALUT stuff (non-legacy) */
  message->address_table_lookups_count = 0;
  if( !message->is_legacy ) {
    /* Transaction Context -> tx -> message -> address_table_lookups */
    message->address_table_lookups_count = txn_descriptor->addr_table_lookup_cnt;
    message->address_table_lookups = fd_scratch_alloc( alignof(fd_exec_test_message_address_table_lookup_t), 
                                                       txn_descriptor->addr_table_lookup_cnt * sizeof(fd_exec_test_message_address_table_lookup_t) );
    for( ulong i = 0; i < txn_descriptor->addr_table_lookup_cnt; ++i ) {
      // alut -> account_key
      fd_pubkey_t * alut_key = (fd_pubkey_t *) (txn_payload + address_lookup_tables[i].addr_off);
      memcpy( message->address_table_lookups[i].account_key, alut_key, sizeof(fd_pubkey_t) );

      // Access ALUT account data to access its keys
      FD_BORROWED_ACCOUNT_DECL(addr_lut_rec);
      int err = fd_acc_mgr_view(slot_ctx->acc_mgr, slot_ctx->funk_txn, alut_key, addr_lut_rec);
      if( FD_UNLIKELY( err != FD_ACC_MGR_SUCCESS ) ) {
        FD_LOG_ERR(( "addr lut not found" ));
      }

      // alut -> writable_indexes
      message->address_table_lookups[i].writable_indexes_count = address_lookup_tables[i].writable_cnt;
      message->address_table_lookups[i].writable_indexes = fd_scratch_alloc( alignof(uint32_t), address_lookup_tables[i].writable_cnt * sizeof(uint32_t) );
      uchar * writable_indexes = (uchar *) (txn_payload + address_lookup_tables[i].writable_off);
      for( ulong j = 0; j < address_lookup_tables[i].writable_cnt; ++j ) {
        message->address_table_lookups[i].writable_indexes[j] = writable_indexes[j];
      }

      // alut -> readonly_indexes
      message->address_table_lookups[i].readonly_indexes_count = address_lookup_tables[i].readonly_cnt;
      message->address_table_lookups[i].readonly_indexes = fd_scratch_alloc( alignof(uint32_t), address_lookup_tables[i].readonly_cnt * sizeof(uint32_t) );
      uchar * readonly_indexes = (uchar *) (txn_payload + address_lookup_tables[i].readonly_off);
      for( ulong j = 0; j < address_lookup_tables[i].readonly_cnt; ++j ) {
        message->address_table_lookups[i].readonly_indexes[j] = readonly_indexes[j];
      }
    }
  }

  /* Transaction Context -> tx -> message_hash */
  // Skip because it does not matter what's in here

  /* Transaction Context -> tx -> is_simple_vote_tx */
  // Doesn't matter for FD, but does for Agave
  // https://github.com/anza-xyz/agave/blob/9a7bf72940f4b3cd7fc94f54e005868ce707d53d/sdk/src/simple_vote_transaction_checker.rs#L5-L9
  sanitized_transaction->is_simple_vote_tx = ( txn_descriptor->signature_cnt < 3 )
                                          && ( message->is_legacy )
                                          && ( txn_descriptor->instr_cnt == 1 )
                                          && ( 0 == memcmp( &txn_ctx->accounts[txn_descriptor->instr[0].program_id], fd_solana_vote_program_id.key, sizeof(fd_pubkey_t) ) );

  /* Transaction Context -> tx -> signatures */
  sanitized_transaction->signatures_count = txn_descriptor->signature_cnt;
  sanitized_transaction->signatures = fd_scratch_alloc( alignof(pb_bytes_array_t *), PB_BYTES_ARRAY_T_ALLOCSIZE(txn_descriptor->signature_cnt * sizeof(pb_bytes_array_t *)) );
  fd_ed25519_sig_t const * signatures = fd_txn_get_signatures( txn_descriptor, txn_payload );
  for( uchar i = 0; i < txn_descriptor->signature_cnt; ++i ) {
    pb_bytes_array_t * signature = fd_scratch_alloc( alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE(sizeof(fd_ed25519_sig_t)) );
    signature->size = sizeof(fd_ed25519_sig_t);
    memcpy( signature->bytes, &signatures[i], sizeof(fd_ed25519_sig_t) );
    sanitized_transaction->signatures[i] = signature;
  }

  /* Transaction Context -> max_age */
  ulong max_age = slot_ctx->slot_bank.block_hash_queue.max_age;
  txn_context_msg->max_age = max_age;

  /* Transaction Context -> blockhash_queue 
     NOTE: Agave's implementation of register_hash incorrectly allows the blockhash queue to hold max_age + 1 (max 301) 
     entries. We have this incorrect logic implemented in fd_sysvar_recent_hashes:register_blockhash and it's not a 
     huge issue, but something to keep in mind. */
  pb_bytes_array_t ** output_blockhash_queue = fd_scratch_alloc( 
                                                      alignof(pb_bytes_array_t *), 
                                                      PB_BYTES_ARRAY_T_ALLOCSIZE((max_age + 1) * sizeof(pb_bytes_array_t *)) );
  txn_context_msg->blockhash_queue_count = 0;
  txn_context_msg->blockhash_queue = output_blockhash_queue;

  // Iterate over all block hashes in the queue and save them
  fd_block_hash_queue_t * queue = &slot_ctx->slot_bank.block_hash_queue;
  fd_hash_hash_age_pair_t_mapnode_t * nn;
  for ( fd_hash_hash_age_pair_t_mapnode_t * n = fd_hash_hash_age_pair_t_map_minimum( queue->ages_pool, queue->ages_root ); n; n = nn ) {
    nn = fd_hash_hash_age_pair_t_map_successor( queue->ages_pool, n );

    /* Get the index in the blockhash queue 
       - Lower index = newer
       - 0 will be the most recent blockhash
       - Index range is [0, max_age] (not a typo) */
    ulong queue_index = queue->last_hash_index - n->elem.val.hash_index;
    fd_hash_t blockhash = n->elem.key;

    // Write the blockhash to the correct index (note we write in reverse order since in the Protobuf message, the oldest blockhash goes first)
    pb_bytes_array_t * output_blockhash = fd_scratch_alloc( alignof(pb_bytes_array_t), PB_BYTES_ARRAY_T_ALLOCSIZE(sizeof(fd_hash_t)) );
    output_blockhash->size = sizeof(fd_hash_t);
    memcpy( output_blockhash->bytes, &blockhash, sizeof(fd_hash_t) );
    output_blockhash_queue[max_age - queue_index] = output_blockhash;
    txn_context_msg->blockhash_queue_count++;
  }

  // Shift blockhash queue elements if num elements < 301
  if( txn_context_msg->blockhash_queue_count < max_age + 1 ) {
    ulong index_offset = max_age + 1 - txn_context_msg->blockhash_queue_count;
    for( ulong i = 0; i < txn_context_msg->blockhash_queue_count; i++ ) {
      output_blockhash_queue[i] = output_blockhash_queue[i + index_offset];
    }
  }

  /* Transaction Context -> epoch_ctx */
  txn_context_msg->has_epoch_ctx = true;
  txn_context_msg->epoch_ctx.has_features = true;
  dump_sorted_features( &txn_ctx->epoch_ctx->features, &txn_context_msg->epoch_ctx.features );

  /* Transaction Context -> slot_ctx */
  txn_context_msg->has_slot_ctx  = true;
  txn_context_msg->slot_ctx.slot = slot_ctx->slot_bank.slot;
}

/*  Similar to dump_instr_to_protobuf, but dumps individual transactions from a ledger replay.

    This is more reliable for BPF program invocations since solfuzz-agave's transaction replay harness
    is more robust.

    The following arguments can be added when replaying ledger transactions:
      --dump-txn-to-pb <0/1>
        * If enabled, transactions will be dumped to the specified output directory
      --dump-proto-sig-filter <base_58_enc_sig>
        * If enabled, only transactions with the specified signature will be dumped
        * Provided signature must be base58-encoded
        * Default behavior if signature filter is not provided is to dump EVERY transaction
      --dump-proto-output-dir <output_dir>
        * Each file represents a single transaction as a serialized TxnContext Protobuf message
        * File name format is "txn-<base58_enc_sig>.bin"
*/
void
dump_txn_to_protobuf( fd_exec_txn_ctx_t *txn_ctx ) {
  FD_SCRATCH_SCOPE_BEGIN {
    // Get base58-encoded tx signature
    const fd_ed25519_sig_t * signatures = fd_txn_get_signatures( txn_ctx->txn_descriptor, txn_ctx->_txn_raw->raw );
    fd_ed25519_sig_t signature; fd_memcpy( signature, signatures[0], sizeof(fd_ed25519_sig_t) );
    char encoded_signature[FD_BASE58_ENCODED_64_SZ];
    ulong out_size;
    fd_base58_encode_64( signature, &out_size, encoded_signature );

    if( txn_ctx->capture_ctx->dump_proto_sig_filter ) {
      ulong filter_strlen = (ulong) strlen(txn_ctx->capture_ctx->dump_proto_sig_filter);

      // Terminate early if the signature does not match
      if( memcmp( txn_ctx->capture_ctx->dump_proto_sig_filter, encoded_signature, filter_strlen < out_size ? filter_strlen : out_size ) ) {
        return;
      }
    }

    fd_exec_test_txn_context_t txn_context_msg = FD_EXEC_TEST_TXN_CONTEXT_INIT_DEFAULT;
    create_txn_context_protobuf_from_txn( &txn_context_msg, txn_ctx );

    /* Output to file */
    ulong out_buf_size = 100 * 1024 * 1024;
    uint8_t * out = fd_scratch_alloc( alignof(uint8_t), out_buf_size );
    pb_ostream_t stream = pb_ostream_from_buffer( out, out_buf_size );
    if( pb_encode( &stream, FD_EXEC_TEST_TXN_CONTEXT_FIELDS, &txn_context_msg ) ) {
      char output_filepath[256]; fd_memset( output_filepath, 0, sizeof(output_filepath) );
      char * position = fd_cstr_init( output_filepath );
      position = fd_cstr_append_cstr( position, txn_ctx->capture_ctx->dump_proto_output_dir );
      position = fd_cstr_append_cstr( position, "/txn-" );
      position = fd_cstr_append_cstr( position, encoded_signature );
      position = fd_cstr_append_cstr(position, ".bin");
      fd_cstr_fini(position);

      FILE * file = fopen(output_filepath, "wb");
      if( file ) {
        fwrite( out, 1, stream.bytes_written, file );
        fclose( file );
      }
    }
  } FD_SCRATCH_SCOPE_END;
}

int
fd_execute_txn( fd_exec_txn_ctx_t * txn_ctx ) {
  FD_SCRATCH_SCOPE_BEGIN {
    uint use_sysvar_instructions = fd_executor_txn_uses_sysvar_instructions( txn_ctx );

    for( ushort i = 0; i < txn_ctx->txn_descriptor->instr_cnt; i++ ) {
      fd_txn_instr_t const * txn_instr = &txn_ctx->txn_descriptor->instr[i];
      fd_convert_txn_instr_to_instr( txn_ctx, txn_instr, txn_ctx->borrowed_accounts, &txn_ctx->instr_infos[i] );
    }

    txn_ctx->instr_info_cnt = txn_ctx->txn_descriptor->instr_cnt;
    if( FD_UNLIKELY( txn_ctx->instr_info_cnt>FD_MAX_INSTRUCTION_TRACE_LENGTH ) ) {
      return FD_EXECUTOR_INSTR_ERR_MAX_INSN_TRACE_LENS_EXCEEDED;
    }

    int ret = 0;

    if ( FD_UNLIKELY( use_sysvar_instructions ) ) {
      ret = fd_sysvar_instructions_serialize_account( txn_ctx, (fd_instr_info_t const *)txn_ctx->instr_infos, txn_ctx->txn_descriptor->instr_cnt );
      if( ret != FD_ACC_MGR_SUCCESS ) {
        FD_LOG_WARNING(( "sysvar instrutions failed to serialize" ));
        return ret;
      }
    }

#ifdef VLOG
    fd_txn_t const *txn = txn_ctx->txn_descriptor;
    fd_rawtxn_b_t const *raw_txn = txn_ctx->_txn_raw;
    uchar * sig = (uchar *)raw_txn->raw + txn->signature_off;
#endif

    bool dump_insn = txn_ctx->capture_ctx && txn_ctx->slot_ctx->slot_bank.slot >= txn_ctx->capture_ctx->dump_proto_start_slot && txn_ctx->capture_ctx->dump_insn_to_pb;

    /* Initialize log collection */
    fd_log_collector_init( &txn_ctx->log_collector, txn_ctx->slot_ctx->enable_exec_recording );

    for ( ushort i = 0; i < txn_ctx->txn_descriptor->instr_cnt; i++ ) {
#ifdef VLOG
      FD_LOG_WARNING(("Start of transaction for %d for %64J", i, sig));
#endif

      if ( FD_UNLIKELY( use_sysvar_instructions ) ) {
        ret = fd_sysvar_instructions_update_current_instr_idx( txn_ctx, i );
        if( ret != FD_ACC_MGR_SUCCESS ) {
          FD_LOG_WARNING(( "sysvar instructions failed to update instruction index" ));
          return ret;
        }
      }

      if( dump_insn ) {
        // Capture the input and convert it into a Protobuf message
        dump_instr_to_protobuf(txn_ctx, &txn_ctx->instr_infos[i], i);
      }


      int exec_result = fd_execute_instr( txn_ctx, &txn_ctx->instr_infos[i] );
#ifdef VLOG
      FD_LOG_WARNING(( "fd_execute_instr result (%d) for %64J", exec_result, sig ));
#endif
      if( exec_result != FD_EXECUTOR_INSTR_SUCCESS ) {
        if ( txn_ctx->instr_err_idx == INT_MAX )
        {
          txn_ctx->instr_err_idx = i;
        }
  #ifdef VLOG
        if ( 257037453 == txn_ctx->slot_ctx->slot_bank.slot ) {
  #endif
          if (exec_result == FD_EXECUTOR_INSTR_ERR_CUSTOM_ERR ) {
  #ifdef VLOG
            FD_LOG_WARNING(( "fd_execute_instr failed (%d:%d) for %64J", exec_result, txn_ctx->custom_err, sig ));
  #endif
          } else {
  #ifdef VLOG
            FD_LOG_WARNING(( "fd_execute_instr failed (%d) index %u for %64J", exec_result, i, sig ));
  #endif
          }
  #ifdef VLOG
        }
  #endif
        if ( FD_UNLIKELY( use_sysvar_instructions ) ) {
          ret = fd_sysvar_instructions_cleanup_account( txn_ctx );
          if( ret != FD_ACC_MGR_SUCCESS ) {
            FD_LOG_WARNING(( "sysvar instructions failed to cleanup" ));
            return ret;
          }
        }
        return exec_result;
      }
    }
    int err = fd_executor_txn_check( txn_ctx->slot_ctx, txn_ctx );
    if ( err != FD_EXECUTOR_INSTR_SUCCESS) {
      FD_LOG_DEBUG(( "fd_executor_txn_check failed (%d)", err ));
      if ( FD_UNLIKELY( use_sysvar_instructions ) ) {
        ret = fd_sysvar_instructions_cleanup_account( txn_ctx );
        if( ret != FD_ACC_MGR_SUCCESS ) {
          FD_LOG_WARNING(( "sysvar instructions failed to cleanup" ));
          return ret;
        }
      }
      return err;
    }

    if ( FD_UNLIKELY( use_sysvar_instructions ) ) {
      ret = fd_sysvar_instructions_cleanup_account( txn_ctx );
      if( ret != FD_ACC_MGR_SUCCESS ) {
        FD_LOG_WARNING(( "sysvar instructions failed to cleanup" ));
        return ret;
      }
    }
    return 0;
  } FD_SCRATCH_SCOPE_END;
}

int fd_executor_txn_check( fd_exec_slot_ctx_t * slot_ctx,  fd_exec_txn_ctx_t *txn ) {
  fd_rent_t const * rent = fd_sysvar_cache_rent( slot_ctx->sysvar_cache );

  ulong ending_lamports = 0;
  ulong ending_dlen = 0;
  ulong starting_lamports = 0;
  ulong starting_dlen = 0;

  /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L63 */
  for( ulong idx = 0; idx < txn->accounts_cnt; idx++ ) {
    fd_borrowed_account_t * b = &txn->borrowed_accounts[idx];

    // Was this account written to?
    if( NULL != b->meta ) {
      ending_lamports += b->meta->info.lamports;
      ending_dlen += b->meta->dlen;

      /* Rent states are defined as followed:
         - lamports == 0                      -> Uninitialized 
         - 0 < lamports < rent_exempt_minimum -> RentPaying 
         - lamports >= rent_exempt_minimum    -> RentExempt
         In Agave, 'self' refers to our 'after' state. */
      uchar after_uninitialized  = b->meta->info.lamports == 0;
      uchar after_rent_exempt    = b->meta->info.lamports >= fd_rent_exempt_minimum_balance2( rent, b->meta->dlen );

      /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L96 */
      if( FD_LIKELY( memcmp( b->pubkey->key, fd_sysvar_incinerator_id.key, sizeof(fd_pubkey_t) ) != 0 ) ) {
        /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L44 */
        if( after_uninitialized || after_rent_exempt ) {
          // no-op
        } else {
          /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L45-L59 */
          uchar before_uninitialized = b->starting_dlen == ULONG_MAX || b->starting_lamports == 0;
          uchar before_rent_exempt   = b->starting_dlen != ULONG_MAX && b->starting_lamports >= fd_rent_exempt_minimum_balance2( rent, b->starting_dlen );

          /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L50 */
          if( before_uninitialized || before_rent_exempt ) {
            FD_LOG_DEBUG(( "Rent exempt error for %32J Curr len %lu Starting len %lu Curr lamports %lu Starting lamports %lu Curr exempt %lu Starting exempt %lu", b->pubkey->uc, b->meta->dlen, b->starting_dlen, b->meta->info.lamports, b->starting_lamports, fd_rent_exempt_minimum_balance2( rent, b->meta->dlen ), fd_rent_exempt_minimum_balance2( rent, b->starting_dlen ) ));
            /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L104 */
            return FD_RUNTIME_TXN_ERR_INSUFFICIENT_FUNDS_FOR_RENT;
          /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L56 */
          } else if( (b->meta->dlen == b->starting_dlen) && b->meta->info.lamports <= b->starting_lamports ) {
            // no-op
          } else {
            FD_LOG_DEBUG(( "Rent exempt error for %32J Curr len %lu Starting len %lu Curr lamports %lu Starting lamports %lu Curr exempt %lu Starting exempt %lu", b->pubkey->uc, b->meta->dlen, b->starting_dlen, b->meta->info.lamports, b->starting_lamports, fd_rent_exempt_minimum_balance2( rent, b->meta->dlen ), fd_rent_exempt_minimum_balance2( rent, b->starting_dlen ) ));
            /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/account_rent_state.rs#L104 */
            return FD_RUNTIME_TXN_ERR_INSUFFICIENT_FUNDS_FOR_RENT;
          }
        }
      }

      if (b->starting_lamports != ULONG_MAX)
        starting_lamports += b->starting_lamports;
      if (b->starting_dlen != ULONG_MAX)
        starting_dlen += b->starting_dlen;
    } else if (NULL != b->const_meta) {
      // FD_LOG_DEBUG(("Const rec mismatch %32J starting %lu %lu ending %lu %lu", b->pubkey->uc, b->starting_dlen, b->starting_lamports, b->const_meta->dlen, b->const_meta->info.lamports));
      // Should these just kill the client?  They are impossible...
      if (b->starting_lamports != b->const_meta->info.lamports) {
        FD_LOG_DEBUG(("Const rec mismatch %32J starting %lu %lu ending %lu %lu", b->pubkey->uc, b->starting_dlen, b->starting_lamports, b->const_meta->dlen, b->const_meta->info.lamports));
        return FD_EXECUTOR_INSTR_ERR_UNBALANCED_INSTR;
      }
      if (b->starting_dlen != b->const_meta->dlen) {
        FD_LOG_DEBUG(("Const rec mismatch %32J starting %lu %lu ending %lu %lu", b->pubkey->uc, b->starting_dlen, b->starting_lamports, b->const_meta->dlen, b->const_meta->info.lamports));
        return FD_EXECUTOR_INSTR_ERR_UNBALANCED_INSTR;
      }
    }
  }

  /* https://github.com/anza-xyz/agave/blob/b2c388d6cbff9b765d574bbb83a4378a1fc8af32/svm/src/transaction_processor.rs#L839-L845
     Should these just kill the client?  They are impossible yet solana just throws an error */
  if (ending_lamports != starting_lamports) {
    FD_LOG_DEBUG(("Lamport sum mismatch: starting %lu ending %lu", starting_lamports, ending_lamports));
    return FD_EXECUTOR_INSTR_ERR_UNBALANCED_INSTR;
  }

  /* TODO unused variables */
  (void)ending_dlen; (void)starting_dlen;

  return FD_EXECUTOR_INSTR_SUCCESS;
}
#undef VLOG

/* fd_executor_instr_strerror() returns the error message corresponding to err,
   intended to be logged by log_collector, or an empty string if the error code
   should be omitted in logs for whatever reason.  Omitted examples are success,
   fatal (placeholder just in firedancer), custom error.
   See also fd_log_collector_program_failure(). */
FD_FN_CONST char const *
fd_executor_instr_strerror( int err ) {

  switch( err ) {
  case FD_EXECUTOR_INSTR_SUCCESS                                : return ""; // not used
  case FD_EXECUTOR_INSTR_ERR_FATAL                              : return ""; // not used
  case FD_EXECUTOR_INSTR_ERR_GENERIC_ERR                        : return "generic instruction error";
  case FD_EXECUTOR_INSTR_ERR_INVALID_ARG                        : return "invalid program argument";
  case FD_EXECUTOR_INSTR_ERR_INVALID_INSTR_DATA                 : return "invalid instruction data";
  case FD_EXECUTOR_INSTR_ERR_INVALID_ACC_DATA                   : return "invalid account data for instruction";
  case FD_EXECUTOR_INSTR_ERR_ACC_DATA_TOO_SMALL                 : return "account data too small for instruction";
  case FD_EXECUTOR_INSTR_ERR_INSUFFICIENT_FUNDS                 : return "insufficient funds for instruction";
  case FD_EXECUTOR_INSTR_ERR_INCORRECT_PROGRAM_ID               : return "incorrect program id for instruction";
  case FD_EXECUTOR_INSTR_ERR_MISSING_REQUIRED_SIGNATURE         : return "missing required signature for instruction";
  case FD_EXECUTOR_INSTR_ERR_ACC_ALREADY_INITIALIZED            : return "instruction requires an uninitialized account";
  case FD_EXECUTOR_INSTR_ERR_UNINITIALIZED_ACCOUNT              : return "instruction requires an initialized account";
  case FD_EXECUTOR_INSTR_ERR_UNBALANCED_INSTR                   : return "sum of account balances before and after instruction do not match";
  case FD_EXECUTOR_INSTR_ERR_MODIFIED_PROGRAM_ID                : return "instruction illegally modified the program id of an account";
  case FD_EXECUTOR_INSTR_ERR_EXTERNAL_ACCOUNT_LAMPORT_SPEND     : return "instruction spent from the balance of an account it does not own";
  case FD_EXECUTOR_INSTR_ERR_EXTERNAL_DATA_MODIFIED             : return "instruction modified data of an account it does not own";
  case FD_EXECUTOR_INSTR_ERR_READONLY_LAMPORT_CHANGE            : return "instruction changed the balance of a read-only account";
  case FD_EXECUTOR_INSTR_ERR_READONLY_DATA_MODIFIED             : return "instruction modified data of a read-only account";
  case FD_EXECUTOR_INSTR_ERR_DUPLICATE_ACCOUNT_IDX              : return "instruction contains duplicate accounts";
  case FD_EXECUTOR_INSTR_ERR_EXECUTABLE_MODIFIED                : return "instruction changed executable bit of an account";
  case FD_EXECUTOR_INSTR_ERR_RENT_EPOCH_MODIFIED                : return "instruction modified rent epoch of an account";
  case FD_EXECUTOR_INSTR_ERR_NOT_ENOUGH_ACC_KEYS                : return "insufficient account keys for instruction";
  case FD_EXECUTOR_INSTR_ERR_ACC_DATA_SIZE_CHANGED              : return "program other than the account's owner changed the size of the account data";
  case FD_EXECUTOR_INSTR_ERR_ACC_NOT_EXECUTABLE                 : return "instruction expected an executable account";
  case FD_EXECUTOR_INSTR_ERR_ACC_BORROW_FAILED                  : return "instruction tries to borrow reference for an account which is already borrowed";
  case FD_EXECUTOR_INSTR_ERR_ACC_BORROW_OUTSTANDING             : return "instruction left account with an outstanding borrowed reference";
  case FD_EXECUTOR_INSTR_ERR_DUPLICATE_ACCOUNT_OUT_OF_SYNC      : return "instruction modifications of multiply-passed account differ";
  case FD_EXECUTOR_INSTR_ERR_CUSTOM_ERR                         : return ""; // custom handling via txn_ctx->custom_err
  case FD_EXECUTOR_INSTR_ERR_INVALID_ERR                        : return "program returned invalid error code";
  case FD_EXECUTOR_INSTR_ERR_EXECUTABLE_DATA_MODIFIED           : return "instruction changed executable accounts data";
  case FD_EXECUTOR_INSTR_ERR_EXECUTABLE_LAMPORT_CHANGE          : return "instruction changed the balance of an executable account";
  case FD_EXECUTOR_INSTR_ERR_EXECUTABLE_ACCOUNT_NOT_RENT_EXEMPT : return "executable accounts must be rent exempt";
  case FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_PROGRAM_ID             : return "Unsupported program id";
  case FD_EXECUTOR_INSTR_ERR_CALL_DEPTH                         : return "Cross-program invocation call depth too deep";
  case FD_EXECUTOR_INSTR_ERR_MISSING_ACC                        : return "An account required by the instruction is missing";
  case FD_EXECUTOR_INSTR_ERR_REENTRANCY_NOT_ALLOWED             : return "Cross-program invocation reentrancy not allowed for this instruction";
  case FD_EXECUTOR_INSTR_ERR_MAX_SEED_LENGTH_EXCEEDED           : return "Length of the seed is too long for address generation";
  case FD_EXECUTOR_INSTR_ERR_INVALID_SEEDS                      : return "Provided seeds do not result in a valid address";
  case FD_EXECUTOR_INSTR_ERR_INVALID_REALLOC                    : return "Failed to reallocate account data";
  case FD_EXECUTOR_INSTR_ERR_COMPUTE_BUDGET_EXCEEDED            : return "Computational budget exceeded";
  case FD_EXECUTOR_INSTR_ERR_PRIVILEGE_ESCALATION               : return "Cross-program invocation with unauthorized signer or writable account";
  case FD_EXECUTOR_INSTR_ERR_PROGRAM_ENVIRONMENT_SETUP_FAILURE  : return "Failed to create program execution environment";
  case FD_EXECUTOR_INSTR_ERR_PROGRAM_FAILED_TO_COMPLETE         : return "Program failed to complete";
  case FD_EXECUTOR_INSTR_ERR_PROGRAM_FAILED_TO_COMPILE          : return "Program failed to compile";
  case FD_EXECUTOR_INSTR_ERR_ACC_IMMUTABLE                      : return "Account is immutable";
  case FD_EXECUTOR_INSTR_ERR_INCORRECT_AUTHORITY                : return "Incorrect authority provided";
  case FD_EXECUTOR_INSTR_ERR_BORSH_IO_ERROR                     : return "Failed to serialize or deserialize account data"; // truncated
  case FD_EXECUTOR_INSTR_ERR_ACC_NOT_RENT_EXEMPT                : return "An account does not have enough lamports to be rent-exempt";
  case FD_EXECUTOR_INSTR_ERR_INVALID_ACC_OWNER                  : return "Invalid account owner";
  case FD_EXECUTOR_INSTR_ERR_ARITHMETIC_OVERFLOW                : return "Program arithmetic overflowed";
  case FD_EXECUTOR_INSTR_ERR_UNSUPPORTED_SYSVAR                 : return "Unsupported sysvar";
  case FD_EXECUTOR_INSTR_ERR_ILLEGAL_OWNER                      : return "Provided owner is not allowed";
  case FD_EXECUTOR_INSTR_ERR_MAX_ACCS_DATA_ALLOCS_EXCEEDED      : return "Accounts data allocations exceeded the maximum allowed per transaction";
  case FD_EXECUTOR_INSTR_ERR_MAX_ACCS_EXCEEDED                  : return "Max accounts exceeded";
  case FD_EXECUTOR_INSTR_ERR_MAX_INSN_TRACE_LENS_EXCEEDED       : return "Max instruction trace length exceeded";
  case FD_EXECUTOR_INSTR_ERR_BUILTINS_MUST_CONSUME_CUS          : return "Builtin programs must consume compute units";
  default: break;
  }

  return "";
}

// This is purely linker magic to force the inclusion of the yaml type walker so that it is
// available for debuggers
void
fd_debug_symbology(void) {
  (void)fd_get_types_yaml();
}
