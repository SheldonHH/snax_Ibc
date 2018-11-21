#include "platform.hpp"

using namespace std;

namespace snax {

    /// @abi action initialize
    void platform::initialize(const account_name token_dealer, const string token_symbol_str, const uint8_t precision) {
        const auto token_symbol = string_to_symbol(precision, token_symbol_str.c_str());

        require_auth(_self);

        snax_assert(_states.find(_self) == _states.end(), "platform is already initialized");

        _states.emplace(_self, [&](auto& state) {
            state.round_supply = asset(0, token_symbol);
            state.step_number = 0;
            state.token_dealer = token_dealer;
            state.total_attention_rate = 0.0;
            state.round_updated_account_count = 0;
            state.updating = 0;
            state.account = _self;
        });
    }

    /// @abi action lockupdate
    void platform::lockupdate() {
        require_auth(_self);
        snax_assert(_states.find(_self) != _states.end(), "platform must be initialized");
        _states.modify(
                _states.find(_self), _self, [&](auto &record) {
                    record.updating = 1;
                }
        );
    }

    /// @abi action nextround
    void platform::nextround() {
        require_auth(_self);
        auto state = get_state();
        snax_assert(state.updating == 1, "platform must be in updating state when nextround action is called");

        action(permission_level{_self, N(active)}, state.token_dealer, N(emitplatform), make_tuple(_self)).send();

        update_state_next_round();
    }


    /// @abi action sendpayments
    void platform::sendpayments(const uint64_t serial_num, uint64_t account_count) {
        require_auth(_self);
        auto state = get_state();

        snax_assert(state.updating == 2, "platform must be in updating state and nextround must be called before sending payments");

        const auto account_serial_index = _accounts.get_index<N(serial)>();
        auto iter = account_serial_index.find(serial_num);
        const auto& end_iter = account_serial_index.cend();
        const auto total_accounts_to_update = account_count;

        snax_assert(iter != end_iter, "cant find account with this serial number");

        asset current_balance = get_balance();

        while (iter != end_iter && account_count--) {
            const auto& account = *iter;
            if (iter == account_serial_index.cbegin()) {
                _states.modify(_states.find(_self), _self, [&](auto& state) {
                    state.round_supply = current_balance;
                });
                state = get_state();
            }
            if (account.name != N(snax.saving) && account.attention_rate > 0.1) {
                snax_assert(account.last_updated_step_number < state.step_number, "account already updated");
                asset token_amount;
                const int64_t portion = static_cast<int64_t>(state.total_attention_rate / account.attention_rate);
                if (portion < state.round_supply.amount) {
                    token_amount = state.round_supply / portion;
                    if (token_amount.amount > current_balance.amount) {
                        token_amount = current_balance;
                    }
                } else {
                    token_amount = asset(0);
                }
                if (token_amount.amount > 0) {
                    current_balance -= token_amount;
                    action(permission_level{_self, N(active)},
                           N(snax.token), N(transfer),
                           make_tuple(_self, account.name, token_amount, string("payment for activity"))
                    ).send();
                }
                _accounts.modify(
                    account, _self, [&](auto& account) {
                        account.last_updated_step_number = state.step_number;
                    }
                );
            }
            iter++;
        }

        const auto updated_account_count = total_accounts_to_update - account_count;

        if (iter == end_iter && state.round_updated_account_count + updated_account_count == state.account_count) {
            unlock_update(current_balance);
        } else {
            _states.modify(_states.find(_self), _self, [&](auto& state) {
                state.round_updated_account_count += updated_account_count;
            });
        }
    }

    /// @abi action updatear
    void platform::updatear(uint64_t id, double attention_rate) {
        require_auth(_self);
        const auto &state = get_state();
        snax_assert(!state.updating,
                     "platform mustn't be in updating state when updatear action is called");

        const auto &found = _accounts.find(id);
        snax_assert(found != _accounts.end(), "user doesn't exist in platform");

        const double diff = attention_rate - found->attention_rate;

        snax_assert(diff >= 0 || abs(diff) <= abs(found->attention_rate), "incorrect attention rate");

        update_state_total_attention_rate_and_user_count(diff, 0);

        _accounts.modify(
                found, _self, [&](auto &record) {
                    record.attention_rate = attention_rate;
                }
        );
    }

    /// @abi action updatearmult
    void platform::updatearmult(vector <account_with_attention_rate> &updates) {
        require_auth(_self);
        const auto &state = get_state();
        snax_assert(!state.updating,
                     "platform mustn't be in updating state when updatearmult action is called");

        double total_attention_rate_diff = 0;

        for (auto& update: updates) {
            const auto& account = _accounts.find(update.id);
            if (account != _accounts.end()) {
                const double attention_rate = update.attention_rate;
                const double diff = attention_rate - account->attention_rate;

                snax_assert(diff >= 0 || abs(diff) <= abs(account->attention_rate), "incorrect attention rate");

                _accounts.modify(
                        account, _self, [&](auto &record) {
                            record.attention_rate = attention_rate;
                        }
                );

                total_attention_rate_diff += diff;
            }
        }

        update_state_total_attention_rate_and_user_count(total_attention_rate_diff, 0);
    }

    /// @abi action addaccount
    void platform::addaccount(account_name account, uint64_t id, double attention_rate) {
        require_auth(_self);
        const auto &state = get_state();

        snax_assert(
                !state.updating,
                "platform must not be in updating state when addaccount action is called"
        );
        snax_assert(attention_rate >= 0, "attention rate must be grater than zero or equal to zero");
        const auto& found = _accounts.find(id);
        snax_assert(found == _accounts.end() || found->name == N(snax.saving), "user already exists");

        if (found == _accounts.end()) {
            _accounts.emplace(
                    _self, [&](auto &record) {
                        record.attention_rate = attention_rate;
                        record.id = id;
                        record.name = account;
                        record.serial = state.account_count;
                    }
            );
        } else {
            _accounts.modify(
                    found, _self, [&](auto &record) {
                        const double diff = attention_rate - record.attention_rate;

                        snax_assert(diff >= 0 || abs(diff) <= abs(record.attention_rate), "incorrect attention rate");

                        record.attention_rate = attention_rate;
                        record.name = account;
                    }
            );
        }

        update_state_total_attention_rate_and_user_count(attention_rate, 1);
    };

    /// @abi action addaccount
    void platform::addaccounts(vector<account_to_add> &accounts_to_add) {
        require_auth(_self);
        const auto &state = get_state();

        snax_assert(
                !state.updating,
                "platform must not be in updating state when addaccount action is called"
        );

        double accumulated_attention_rate = 0;
        uint32_t index = 0;

        for (auto& account_to_add: accounts_to_add) {
            snax_assert(_accounts.find(account_to_add.id) == _accounts.end(), "user already exists");
            snax_assert(account_to_add.attention_rate >= 0, "attention rate must be grater than zero");
            accumulated_attention_rate += account_to_add.attention_rate;
            _accounts.emplace(
                    _self, [&](auto &record) {
                        record.attention_rate = account_to_add.attention_rate;
                        record.id = account_to_add.id;
                        record.name = account_to_add.name;
                        record.serial = state.account_count + index;
                    }
            );
            index++;
        }

        update_state_total_attention_rate_and_user_count(accumulated_attention_rate, accounts_to_add.size());
    }

    asset platform::get_balance() {
        const auto& state = get_state();
        _accounts_balances balances(N(snax.token), _self);
        const auto platform_balance = *balances.find(state.round_supply.symbol.name());
        return platform_balance.balance;
    }

    void platform::update_state_next_round() {
        const auto &found = _states.find(_self);

        _states.modify(
            found, _self, [&](auto& state) {
                state.step_number++;
                state.round_updated_account_count = 0;
                state.updating = 2;
            }
        );
    }

    void platform::update_state_total_attention_rate_and_user_count(double additional_attention_rate, uint64_t new_accounts) {
        const auto &found = _states.find(_self);
        snax_assert(found != _states.end(), "platform isn't initialized");
        _states.modify(
                found, _self, [&](auto &record) {
                    record.total_attention_rate += additional_attention_rate;
                    record.account_count += new_accounts;
                }
        );
    }

    // Only contract itself is allowed to unlock update
    void platform::unlock_update(const asset current_amount) {
        const auto& found = _states.find(_self);
        snax_assert(found != _states.end(), "platform isn't initialized");
        _states.modify(
                found, _self, [&](auto &state) {
                    state.updating = 0;
                    state.round_updated_account_count;
                    state.round_supply -= state.round_supply;
                }
        );
        if (current_amount.amount > 0) {
            action(permission_level{_self, N(active)}, N(snax.token), N(transfer), make_tuple(_self, N(snax), current_amount, string("rest of money"))).send();
        }
    }

    platform::state platform::get_state() {
        const auto &found = _states.find(_self);
        snax_assert(found != _states.end(), "platform isn't initialized");
        return *found;
    }

    platform::account platform::find_account(account_name account) {
        const auto account_index = _accounts.get_index<N(name)>();
        const auto& found = account_index.find(account);
        snax_assert(found != account_index.end(), "user doesn't exist in platform");

        return *found;
    }

    double platform::get_token_portion(
            double _player_rate,
            double _total_attention_rate
    ) const {
        return _player_rate / _total_attention_rate;
    }

    double platform::convert_asset_to_double(const asset value) const {
        return static_cast<double>(value.amount);
    }

}

SNAX_ABI(snax::platform, (initialize)(lockupdate)(nextround)(sendpayments)(addaccount)(addaccounts)(updatear)
(updatearmult))