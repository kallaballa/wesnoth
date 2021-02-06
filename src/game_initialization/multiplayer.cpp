/*
   Copyright (C) 2007 - 2018 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project https://www.wesnoth.org

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "game_initialization/multiplayer.hpp"

#include "addon/manager.hpp" // for installed_addons
#include "build_info.hpp"
#include "commandline_options.hpp"
#include "connect_engine.hpp"
#include "events.hpp"
#include "formula/string_utils.hpp"
#include "game_config_manager.hpp"
#include "game_initialization/mp_game_utils.hpp"
#include "game_initialization/playcampaign.hpp"
#include "gettext.hpp"
#include "gui/dialogs/loading_screen.hpp"
#include "gui/dialogs/message.hpp"
#include "gui/dialogs/multiplayer/lobby.hpp"
#include "gui/dialogs/multiplayer/mp_create_game.hpp"
#include "gui/dialogs/multiplayer/mp_join_game.hpp"
#include "gui/dialogs/multiplayer/mp_login.hpp"
#include "gui/dialogs/multiplayer/mp_staging.hpp"
#include "hash.hpp"
#include "log.hpp"
#include "map_settings.hpp"
#include "multiplayer_error_codes.hpp"
#include "preferences/credentials.hpp"
#include "preferences/game.hpp"
#include "replay.hpp"
#include "resources.hpp"
#include "saved_game.hpp"
#include "sound.hpp"
#include "statistics.hpp"
#include "utils/parse_network_address.hpp"
#include "wesnothd_connection.hpp"

#include <fstream>
#include <functional>
#include <future>
#include <optional>
#include <thread>

static lg::log_domain log_mp("mp/main");
#define DBG_MP LOG_STREAM(debug, log_mp)
#define ERR_MP LOG_STREAM(err, log_mp)

namespace mp
{
namespace
{
/** Pointer to the current mp_manager instance. */
class mp_manager* manager = nullptr;

/** The main controller of the MP workflow. */
class mp_manager
{
public:
	// Declare this as a friend to allow direct access to enter_create_mode
	friend void mp::start_local_game();

	mp_manager(const std::optional<std::string> host);

	~mp_manager()
	{
		assert(manager);
		manager = nullptr;

		if(network_worker.joinable()) {
			stop = true;
			network_worker.join();
		}
	}

	/**
	 * Enters the mp loop. It consists of four screens:
	 *
	 * Host POV:   LOBBY <---> CREATE GAME ---> STAGING -----> GAME BEGINS
	 * Player POV: LOBBY <--------------------> JOIN GAME ---> GAME BEGINS
	 */
	void run_lobby_loop();

	bool post_scenario_staging(ng::connect_engine& engine);

	bool post_scenario_wait(bool observe);

private:
	/** Represents the contents of the [join_lobby] response. */
	struct session_metadata
	{
		session_metadata() = default;

		session_metadata(const config& cfg)
			: is_moderator(cfg["is_moderator"].to_bool(false))
			, profile_url_prefix(cfg["profile_url_prefix"].str())
		{
		}

		/** Whether you are logged in as a server moderator. */
		bool is_moderator = false;

		/** The external URL prefix for player profiles (empty if the server doesn't have an attached database). */
		std::string profile_url_prefix;
	};

	/** Opens a new server connection and prompts the client for login credentials, if necessary. */
	std::unique_ptr<wesnothd_connection> open_connection(std::string host);

	/** Opens the MP lobby. */
	bool enter_lobby_mode();

	/** Opens the MP Create screen for hosts to configure a new game. */
	void enter_create_mode();

	/** Opens the MP Staging screen for hosts to wait for players. */
	void enter_staging_mode();

	/** Opens the MP Join Game screen for non-host players and observers. */
	void enter_wait_mode(int game_id, bool observe);

	/** Worker thread to handle receiving and processing network data. */
	std::thread network_worker;

	/** Flag to signal the worker thread terminate. */
	std::atomic_bool stop;

	/** The connection to the server. */
	std::unique_ptr<wesnothd_connection> connection;

	/** The current session's info sent by the server on login. */
	session_metadata session_info;

	/** This single instance is reused for all games played during the current connection to the server. */
	saved_game state;

	mp::lobby_info lobby_info;

public:
	const session_metadata& get_session_info() const
	{
		return session_info;
	}
};

mp_manager::mp_manager(const std::optional<std::string> host)
	: network_worker()
	, stop(false)
	, connection(nullptr)
	, session_info()
	, state()
	, lobby_info(::installed_addons())
{
	state.classification().campaign_type = game_classification::CAMPAIGN_TYPE::MULTIPLAYER;

	if(host) {
		gui2::dialogs::loading_screen::display([&]() {
			connection = open_connection(*host);

			// If for whatever reason our connection is null at this point (dismissing the password prompt, for
			// instance), treat it as a normal condition and exit. Any actual error conditions throw exceptions
			// which can be handled higher up the stack.
			if(connection == nullptr) {
				return;
			}

			gui2::dialogs::loading_screen::progress(loading_stage::download_lobby_data);

			std::promise<void> received_initial_gamelist;

			network_worker = std::thread([this, &received_initial_gamelist]() {
				config data;

				while(!stop) {
					connection->wait_and_receive_data(data);

					if(const config& error = data.child("error")) {
						throw wesnothd_error(error["message"]);
					}

					else if(data.has_child("gamelist")) {
						this->lobby_info.process_gamelist(data);

						try {
							received_initial_gamelist.set_value();
							// TODO: only here while we transition away from dialog-bound timer-based handling
							return;
						} catch(const std::future_error& e) {
							if(e.code() == std::future_errc::promise_already_satisfied) {
								// We only need this for the first gamelist
							}
						}
					}

					else if(const config& gamelist_diff = data.child("gamelist_diff")) {
						this->lobby_info.process_gamelist_diff(gamelist_diff);
					}
				}
			});

			// Wait at the loading screen until the initial gamelist has been processed
			received_initial_gamelist.get_future().wait();
		});
	}

	// Avoid setting this until the connection has been fully established. open_connection may throw,
	// in which case we don't want to point to an object instance that has not properly connected.
	assert(!manager);
	manager = this;
}

std::unique_ptr<wesnothd_connection> mp_manager::open_connection(std::string host)
{
	DBG_MP << "opening connection" << std::endl;

	if(host.empty()) {
		return nullptr;
	}

	// shown_hosts is used to prevent the client being locked in a redirect loop.
	std::set<std::pair<std::string, std::string>> shown_hosts;
	auto addr = shown_hosts.end();

	try {
		std::tie(addr, std::ignore) = shown_hosts.insert(parse_network_address(host, "15000"));
	} catch(const std::runtime_error&) {
		throw wesnothd_error(_("Invalid address specified for multiplayer server"));
	}

	// Start stage
	gui2::dialogs::loading_screen::progress(loading_stage::connect_to_server);

	// Initializes the connection to the server.
	auto conn = std::make_unique<wesnothd_connection>(addr->first, addr->second, true);

	// First, spin until we get a handshake from the server.
	conn->wait_for_handshake();

	gui2::dialogs::loading_screen::progress(loading_stage::waiting);

	config data;

	// Then, log in and wait for the lobby/game join prompt.
	while(true) {
		data.clear();
		conn->wait_and_receive_data(data);

		if(data.has_child("reject") || data.has_attribute("version")) {
			std::string version;

			if(const config& reject = data.child("reject")) {
				version = reject["accepted_versions"].str();
			} else {
				// Backwards-compatibility "version" attribute
				version = data["version"].str();
			}

			utils::string_map i18n_symbols;
			i18n_symbols["required_version"] = version;
			i18n_symbols["your_version"] = game_config::wesnoth_version.str();

			const std::string errorstring = VGETTEXT("The server accepts versions '$required_version', but you are using version '$your_version'", i18n_symbols);
			throw wesnothd_error(errorstring);
		}

		// Check for "redirect" messages
		if(const config& redirect = data.child("redirect")) {
			auto redirect_host = redirect["host"].str();
			auto redirect_port = redirect["port"].str("15000");

			bool recorded_host;
			std::tie(std::ignore, recorded_host) = shown_hosts.emplace(redirect_host, redirect_port);

			if(!recorded_host) {
				throw wesnothd_error(_("Server-side redirect loop"));
			}

			gui2::dialogs::loading_screen::progress(loading_stage::redirect);

			// Open a new connection with the new host and port.
			conn.reset();
			conn = std::make_unique<wesnothd_connection>(redirect_host, redirect_port, true);

			// Wait for new handshake.
			conn->wait_for_handshake();

			gui2::dialogs::loading_screen::progress(loading_stage::waiting);
			continue;
		}

		if(data.has_child("version")) {
			config res;
			config& cfg = res.add_child("version");
			cfg["version"] = game_config::wesnoth_version.str();
			cfg["client_source"] = game_config::dist_channel_id();
			conn->send_data(res);
		}

		if(const config& error = data.child("error")) {
			throw wesnothd_rejected_client_error(error["message"].str());
		}

		// Continue if we did not get a direction to login
		if(!data.has_child("mustlogin")) {
			continue;
		}

		// Enter login loop
		while(true) {
			std::string login = preferences::login();

			config response;
			config& sp = response.add_child("login");
			sp["username"] = login;

			conn->send_data(response);
			conn->wait_and_receive_data(data);

			gui2::dialogs::loading_screen::progress(loading_stage::login_response);

			if(const config& warning = data.child("warning")) {
				std::string warning_msg;

				if(warning["warning_code"] == MP_NAME_INACTIVE_WARNING) {
					warning_msg = VGETTEXT("The nickname ‘$nick’ is inactive. "
						"You cannot claim ownership of this nickname until you "
						"activate your account via email or ask an "
						"administrator to do it for you.", {{"nick", login}});
				} else {
					warning_msg = warning["message"].str();
				}

				warning_msg += "\n\n";
				warning_msg += _("Do you want to continue?");

				if(gui2::show_message(_("Warning"), warning_msg, gui2::dialogs::message::yes_no_buttons) != gui2::retval::OK) {
					return nullptr;
				} else {
					continue;
				}
			}

			config* error = &data.child("error");

			// ... and get us out of here if the server did not complain
			if(!*error) break;

			do {
				std::string password = preferences::password(host, login);

				const bool fall_through = (*error)["force_confirmation"].to_bool()
					? (gui2::show_message(_("Confirm"), (*error)["message"], gui2::dialogs::message::ok_cancel_buttons) == gui2::retval::CANCEL)
					: false;

				const bool is_pw_request = !((*error)["password_request"].empty()) && !(password.empty());

				// If the server asks for a password, provide one if we can
				// or request a password reminder.
				// Otherwise or if the user pressed 'cancel' in the confirmation dialog
				// above go directly to the username/password dialog
				if(is_pw_request && !fall_through) {
					if((*error)["phpbb_encryption"].to_bool()) {
						// Apparently HTML key-characters are passed to the hashing functions of phpbb in this escaped form.
						// I will do closer investigations on this, for now let's just hope these are all of them.

						// Note: we must obviously replace '&' first, I wasted some time before I figured that out... :)
						for(std::string::size_type pos = 0; (pos = password.find('&', pos)) != std::string::npos; ++pos)
							password.replace(pos, 1, "&amp;");
						for(std::string::size_type pos = 0; (pos = password.find('\"', pos)) != std::string::npos; ++pos)
							password.replace(pos, 1, "&quot;");
						for(std::string::size_type pos = 0; (pos = password.find('<', pos)) != std::string::npos; ++pos)
							password.replace(pos, 1, "&lt;");
						for(std::string::size_type pos = 0; (pos = password.find('>', pos)) != std::string::npos; ++pos)
							password.replace(pos, 1, "&gt;");

						const std::string salt = (*error)["salt"];
						if(salt.length() < 12) {
							throw wesnothd_error(_("Bad data received from server"));
						}

						if(utils::md5::is_valid_prefix(salt)) {
							sp["password"] = utils::md5(
								utils::md5(password, utils::md5::get_salt(salt), utils::md5::get_iteration_count(salt)).base64_digest(),
								salt.substr(12, 8)
							).base64_digest();
						} else if(utils::bcrypt::is_valid_prefix(salt)) {
							try {
								auto bcrypt_salt = utils::bcrypt::from_salted_salt(salt);
								auto hash = utils::bcrypt::hash_pw(password, bcrypt_salt);

								const std::string outer_salt = salt.substr(bcrypt_salt.iteration_count_delim_pos + 23);
								if(outer_salt.size() != 32) {
									throw utils::hash_error("salt wrong size");
								}

								sp["password"] = utils::md5(hash.base64_digest(), outer_salt).base64_digest();
							} catch(const utils::hash_error& err) {
								ERR_MP << "bcrypt hash failed: " << err.what() << std::endl;
								throw wesnothd_error(_("Bad data received from server"));
							}
						} else {
							throw wesnothd_error(_("Bad data received from server"));
						}
					} else {
						sp["password"] = password;
					}

					// Once again send our request...
					conn->send_data(response);
					conn->wait_and_receive_data(data);

					gui2::dialogs::loading_screen::progress(loading_stage::login_response);

					error = &data.child("error");

					// ... and get us out of here if the server is happy now
					if(!*error) break;
				}

				// Providing a password either was not attempted because we did not
				// have any or failed:
				// Now show a dialog that displays the error and allows to
				// enter a new user name and/or password

				std::string error_message;
				utils::string_map i18n_symbols;
				i18n_symbols["nick"] = login;

				const bool has_extra_data = error->has_child("data");
				if(has_extra_data) {
					i18n_symbols["duration"] = utils::format_timespan((*error).child("data")["duration"]);
				}

				const std::string ec = (*error)["error_code"];

				if(ec == MP_MUST_LOGIN) {
					error_message = _("You must login first.");
				} else if(ec == MP_NAME_TAKEN_ERROR) {
					error_message = VGETTEXT("The nickname ‘$nick’ is already taken.", i18n_symbols);
				} else if(ec == MP_INVALID_CHARS_IN_NAME_ERROR) {
					error_message = VGETTEXT("The nickname ‘$nick’ contains invalid "
							"characters. Only alpha-numeric characters (one at minimum), underscores and "
							"hyphens are allowed.", i18n_symbols);
				} else if(ec == MP_NAME_TOO_LONG_ERROR) {
					error_message = VGETTEXT("The nickname ‘$nick’ is too long. Nicks must be 20 characters or less.", i18n_symbols);
				} else if(ec == MP_NAME_RESERVED_ERROR) {
					error_message = VGETTEXT("The nickname ‘$nick’ is reserved and cannot be used by players.", i18n_symbols);
				} else if(ec == MP_NAME_UNREGISTERED_ERROR) {
					error_message = VGETTEXT("The nickname ‘$nick’ is not registered on this server.", i18n_symbols)
							+ _(" This server disallows unregistered nicknames.");
				} else if(ec == MP_NAME_AUTH_BAN_USER_ERROR) {
					if(has_extra_data) {
						error_message = VGETTEXT("The nickname ‘$nick’ is banned on this server’s forums for $duration|.", i18n_symbols);
					} else {
						error_message = VGETTEXT("The nickname ‘$nick’ is banned on this server’s forums.", i18n_symbols);
					}
				} else if(ec == MP_NAME_AUTH_BAN_IP_ERROR) {
					if(has_extra_data) {
						error_message = VGETTEXT("Your IP address is banned on this server’s forums for $duration|.", i18n_symbols);
					} else {
						error_message = _("Your IP address is banned on this server’s forums.");
					}
				} else if(ec == MP_NAME_AUTH_BAN_EMAIL_ERROR) {
					if(has_extra_data) {
						error_message = VGETTEXT("The email address for the nickname ‘$nick’ is banned on this server’s forums for $duration|.", i18n_symbols);
					} else {
						error_message = VGETTEXT("The email address for the nickname ‘$nick’ is banned on this server’s forums.", i18n_symbols);
					}
				} else if(ec == MP_PASSWORD_REQUEST) {
					error_message = VGETTEXT("The nickname ‘$nick’ is registered on this server.", i18n_symbols);
				} else if(ec == MP_PASSWORD_REQUEST_FOR_LOGGED_IN_NAME) {
					error_message = VGETTEXT("The nickname ‘$nick’ is registered on this server.", i18n_symbols)
							+ "\n\n" + _("WARNING: There is already a client using this nickname, "
							"logging in will cause that client to be kicked!");
				} else if(ec == MP_NO_SEED_ERROR) {
					error_message = _("Error in the login procedure (the server had no seed for your connection).");
				} else if(ec == MP_INCORRECT_PASSWORD_ERROR) {
					error_message = _("The password you provided was incorrect.");
				} else if(ec == MP_TOO_MANY_ATTEMPTS_ERROR) {
					error_message = _("You have made too many login attempts.");
				} else {
					error_message = (*error)["message"].str();
				}

				gui2::dialogs::mp_login dlg(host, error_message, !((*error)["password_request"].empty()));

				// Need to show the dialog from the main thread or it won't appear.
				events::call_in_main_thread([&dlg]() { dlg.show(); });

				switch(dlg.get_retval()) {
					// Log in with password
					case gui2::retval::OK:
						break;
					// Cancel
					default:
						return nullptr;
				}

			// If we have got a new username we have to start all over again
			} while(login == preferences::login());

			// Somewhat hacky...
			// If we broke out of the do-while loop above error is still going to be nullptr
			if(!*error) break;
		} // end login loop

		if(const config& join_lobby = data.child("join_lobby")) {
			// Note any session data sent with the response. This should be the only place session_info is set.
			session_info = { join_lobby };

			// All done!
			break;
		}
	}

	return conn;
}

void mp_manager::run_lobby_loop()
{
	// This should only work if we have a connection. If we're in a local mode,
	// enter_create_mode should be accessed directly.
	if(!connection) {
		return;
	}

	// A return of false means a config reload was requested, so do that and then loop.
	while(!enter_lobby_mode()) {
		game_config_manager* gcm = game_config_manager::get();
		gcm->reload_changed_game_config();
		gcm->load_game_config_for_create(true); // NOTE: Using reload_changed_game_config only doesn't seem to work here

		// This function does not refer to an addon database, it calls filesystem functions.
		// For the sanity of the mp lobby, this list should be fixed for the entire lobby session,
		// even if the user changes the contents of the addon directory in the meantime.
		// TODO: do we want to handle fetching the installed addons in the lobby_info ctor?
		lobby_info.set_installed_addons(::installed_addons());

		connection->send_data(config("refresh_lobby"));
	}
}

bool mp_manager::enter_lobby_mode()
{
	DBG_MP << "entering lobby mode" << std::endl;

	// Connection should never be null in the lobby.
	assert(connection);

	// We use a loop here to allow returning to the lobby if you, say, cancel game creation.
	while(true) {
		if(const config& cfg = game_config_manager::get()->game_config().child("lobby_music")) {
			for(const config& i : cfg.child_range("music")) {
				sound::play_music_config(i);
			}

			sound::commit_music_changes();
		} else {
			sound::empty_playlist();
			sound::stop_music();
		}

		int dlg_retval = 0;
		int dlg_joined_game_id = 0;
		{
			gui2::dialogs::mp_lobby dlg(lobby_info, *connection, dlg_joined_game_id);
			dlg.show();
			dlg_retval = dlg.get_retval();
		}

		try {
			switch(dlg_retval) {
			case gui2::dialogs::mp_lobby::CREATE:
				enter_create_mode();
				break;
			case gui2::dialogs::mp_lobby::JOIN:
				[[fallthrough]];
			case gui2::dialogs::mp_lobby::OBSERVE:
				enter_wait_mode(dlg_joined_game_id, dlg_retval == gui2::dialogs::mp_lobby::OBSERVE);
				break;
			case gui2::dialogs::mp_lobby::RELOAD_CONFIG:
				// Let this function's caller reload the config and re-call.
				return false;
			default:
				// Needed to handle the Quit signal and exit the loop
				return true;
			}
		} catch(const config::error& error) {
			if(!error.message.empty()) {
				gui2::show_error_message(error.message);
			}

			// Update lobby content
			connection->send_data(config("refresh_lobby"));
		}
	}

	return true;
}

void mp_manager::enter_create_mode()
{
	DBG_MP << "entering create mode" << std::endl;

	if(gui2::dialogs::mp_create_game::execute(state, connection == nullptr)) {
		enter_staging_mode();
	} else if(connection) {
		connection->send_data(config("refresh_lobby"));
	}
}

void mp_manager::enter_staging_mode()
{
	DBG_MP << "entering connect mode" << std::endl;

	std::unique_ptr<mp_game_metadata> metadata;

	// If we have a connection, set the appropriate info. No connection means we're in local game mode.
	if(connection) {
		metadata = std::make_unique<mp_game_metadata>(*connection);
		metadata->connected_players.insert(preferences::login());
		metadata->is_host = true;
	}

	bool dlg_ok = false;
	{
		ng::connect_engine connect_engine(state, true, metadata.get());
		dlg_ok = gui2::dialogs::mp_staging::execute(connect_engine, connection.get());
	} // end connect_engine

	if(dlg_ok) {
		campaign_controller controller(state);
		controller.set_mp_info(metadata.get());
		controller.play_game();
	}

	if(connection) {
		connection->send_data(config("leave_game"));
	}
}

void mp_manager::enter_wait_mode(int game_id, bool observe)
{
	DBG_MP << "entering wait mode" << std::endl;

	// The connection should never be null here, since one should never reach this screen in local game mode.
	assert(connection);

	statistics::fresh_stats();

	mp_game_metadata metadata(*connection);
	metadata.is_host = false;

	if(const mp::game_info* gi = lobby_info.get_game_by_id(game_id)) {
		metadata.current_turn = gi->current_turn;
	}

	if(preferences::skip_mp_replay() || preferences::blindfold_replay()) {
		metadata.skip_replay = true;
		metadata.skip_replay_blindfolded = preferences::blindfold_replay();
	}

	bool dlg_ok = false;
	{
		gui2::dialogs::mp_join_game dlg(state, *connection, true, observe);

		if(!dlg.fetch_game_config()) {
			connection->send_data(config("leave_game"));
			return;
		}

		dlg_ok = dlg.show();
	}

	if(dlg_ok) {
		campaign_controller controller(state);
		controller.set_mp_info(&metadata);
		controller.play_game();
	}

	connection->send_data(config("leave_game"));
}

bool mp_manager::post_scenario_staging(ng::connect_engine& engine)
{
	return gui2::dialogs::mp_staging::execute(engine, connection.get());
}

bool mp_manager::post_scenario_wait(bool observe)
{
	gui2::dialogs::mp_join_game dlg(state, *connection, false, observe);

	if(!dlg.fetch_game_config()) {
		connection->send_data(config("leave_game"));
		return false;
	}

	if(dlg.started()) {
		return true;
	}

	return dlg.show();
}

} // end anon namespace

/** Pubic entry points for the MP workflow */

void start_client(const std::string& host)
{
	DBG_MP << "starting client" << std::endl;
	mp_manager(host).run_lobby_loop();
}

void start_local_game()
{
	DBG_MP << "starting local game" << std::endl;

	preferences::set_message_private(false);

	mp_manager(std::nullopt).enter_create_mode();
}

void start_local_game_commandline(const commandline_options& cmdline_opts)
{
	DBG_MP << "starting local MP game from commandline" << std::endl;

	const game_config_view& game_config = game_config_manager::get()->game_config();

	// The setup is done equivalently to lobby MP games using as much of existing
	// code as possible.  This means that some things are set up that are not
	// needed in commandline mode, but they are required by the functions called.
	preferences::set_message_private(false);

	DBG_MP << "entering create mode" << std::endl;

	// Set the default parameters
	saved_game state;
	state.classification().campaign_type = game_classification::CAMPAIGN_TYPE::MULTIPLAYER;

	mp_game_settings& parameters = state.mp_settings();

	// Hardcoded default values
	state.classification().era_id = "era_default";
	parameters.name = "multiplayer_The_Freelands";

	// Default values for which at getter function exists
	parameters.num_turns = settings::get_turns("");
	parameters.village_gold = settings::get_village_gold("");
	parameters.village_support = settings::get_village_support("");
	parameters.xp_modifier = settings::get_xp_modifier("");

	// Do not use map settings if --ignore-map-settings commandline option is set
	if(cmdline_opts.multiplayer_ignore_map_settings) {
		DBG_MP << "ignoring map settings" << std::endl;
		parameters.use_map_settings = false;
	} else {
		parameters.use_map_settings = true;
	}

	// None of the other parameters need to be set, as their creation values above are good enough for CL mode.
	// In particular, we do not want to use the preferences values.

	state.classification().campaign_type = game_classification::CAMPAIGN_TYPE::MULTIPLAYER;

	// [era] define.
	if(cmdline_opts.multiplayer_era) {
		state.classification().era_id = *cmdline_opts.multiplayer_era;
	}

	if(const config& cfg_era = game_config.find_child("era", "id", state.classification().era_id)) {
		state.classification().era_define = cfg_era["define"].str();
	} else {
		std::cerr << "Could not find era '" << state.classification().era_id << "'\n";
		return;
	}

	// [multiplayer] define.
	if(cmdline_opts.multiplayer_scenario) {
		parameters.name = *cmdline_opts.multiplayer_scenario;
	}

	if(const config& cfg_multiplayer = game_config.find_child("multiplayer", "id", parameters.name)) {
		state.classification().scenario_define = cfg_multiplayer["define"].str();
	} else {
		std::cerr << "Could not find [multiplayer] '" << parameters.name << "'\n";
		return;
	}

	state.set_carryover_sides_start(
		config {"next_scenario", parameters.name}
	);

	game_config_manager::get()->load_game_config_for_game(state.classification(), state.get_scenario_id());

	state.expand_random_scenario();
	state.expand_mp_events();
	state.expand_mp_options();

	// Should number of turns be determined from scenario data?
	if(parameters.use_map_settings && state.get_starting_point()["turns"]) {
		DBG_MP << "setting turns from scenario data: " << state.get_starting_point()["turns"] << std::endl;
		parameters.num_turns = state.get_starting_point()["turns"];
	}

	DBG_MP << "entering connect mode" << std::endl;

	statistics::fresh_stats();

	{
		ng::connect_engine connect_engine(state, true, nullptr);

		// Update the parameters to reflect game start conditions
		connect_engine.start_game_commandline(cmdline_opts, game_config);
	}

	if(resources::recorder && cmdline_opts.multiplayer_label) {
		std::string label = *cmdline_opts.multiplayer_label;
		resources::recorder->add_log_data("ai_log","ai_label",label);
	}

	unsigned int repeat = (cmdline_opts.multiplayer_repeat) ? *cmdline_opts.multiplayer_repeat : 1;
	for(unsigned int i = 0; i < repeat; i++){
		saved_game state_copy(state);
		campaign_controller controller(state_copy);
		controller.play_game();
	}
}

bool goto_mp_staging(ng::connect_engine& engine)
{
	return manager && manager->post_scenario_staging(engine);
}

bool goto_mp_wait(bool observe)
{
	return manager && manager->post_scenario_wait(observe);
}

bool logged_in_as_moderator()
{
	return manager && manager->get_session_info().is_moderator;
}

std::string get_profile_link(int user_id)
{
	if(manager) {
		const std::string& prefix = manager->get_session_info().profile_url_prefix;

		if(!prefix.empty()) {
			return prefix + std::to_string(user_id);
		}
	}

	return "";
}

} // end namespace mp
