// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/cluster_config.hpp"

#include "clustering/administration/admin_op_exc.hpp"
#include "clustering/administration/datum_adapter.hpp"

cluster_config_artificial_table_backend_t::cluster_config_artificial_table_backend_t(
        boost::shared_ptr< semilattice_readwrite_view_t<
            auth_semilattice_metadata_t> > _sl_view) :
    auth_doc(_sl_view) {
    docs["auth"] = &auth_doc;
}

cluster_config_artificial_table_backend_t::~cluster_config_artificial_table_backend_t() {
    begin_changefeed_destruction();
}

std::string cluster_config_artificial_table_backend_t::get_primary_key_name() {
    return "id";
}

bool cluster_config_artificial_table_backend_t::read_all_rows_as_vector(
        signal_t *interruptor,
        std::vector<ql::datum_t> *rows_out,
        admin_err_t *error_out) {
    rows_out->clear();

    for (auto it = docs.begin(); it != docs.end(); ++it) {
        ql::datum_t row;
        if (!it->second->read(interruptor, &row, error_out)) {
            return false;
        }
        rows_out->push_back(row);
    }
    return true;
}

bool cluster_config_artificial_table_backend_t::read_row(
        ql::datum_t primary_key,
        signal_t *interruptor,
        ql::datum_t *row_out,
        admin_err_t *error_out) {
    if (primary_key.get_type() != ql::datum_t::R_STR) {
        *row_out = ql::datum_t();
        return true;
    }
    auto it = docs.find(primary_key.as_str().to_std());
    if (it == docs.end()) {
        *row_out = ql::datum_t();
        return true;
    }
    return it->second->read(interruptor, row_out, error_out);
}

bool cluster_config_artificial_table_backend_t::write_row(
        ql::datum_t primary_key,
        UNUSED bool pkey_was_autogenerated,
        ql::datum_t *new_value_inout,
        signal_t *interruptor,
        admin_err_t *error_out) {
    if (!new_value_inout->has()) {
        *error_out = admin_err_t{
            "It's illegal to delete rows from the `rethinkdb.cluster_config` table.",
            query_state_t::FAILED};
        return false;
    }
    const char *missing_message = "It's illegal to insert new rows into the "
        "`rethinkdb.cluster_config` table.";
    if (primary_key.get_type() != ql::datum_t::R_STR) {
        *error_out = admin_err_t{missing_message, query_state_t::FAILED};
        return false;
    }
    auto it = docs.find(primary_key.as_str().to_std());
    if (it == docs.end()) {
        *error_out = admin_err_t{missing_message, query_state_t::FAILED};
        return false;
    }
    return it->second->write(interruptor, new_value_inout, error_out);
}

void cluster_config_artificial_table_backend_t::set_notifications(bool should_notify) {
    /* Note that we aren't actually modifying the `docs` map itself, just the objects
    that it points at. So this could have been `const auto &pair`, but that might be
    misleading. */
    for (auto &&pair : docs) {
        if (should_notify) {
            std::string name = pair.first;
            pair.second->set_notification_callback(
                [this, name]() {
                    notify_row(ql::datum_t(datum_string_t(name)));
                });
        } else {
            pair.second->set_notification_callback(nullptr);
        }
    }
}

ql::datum_t make_hidden_auth_key_datum() {
    ql::datum_object_builder_t builder;
    builder.overwrite("hidden", ql::datum_t::boolean(true));
    return std::move(builder).to_datum();
}

ql::datum_t convert_auth_key_to_datum(
        const auth_key_t &value) {
    if (value.str().empty()) {
        return ql::datum_t::null();
    } else {
        return make_hidden_auth_key_datum();
    }
}

bool convert_auth_key_from_datum(
        ql::datum_t datum,
        auth_key_t *value_out,
        admin_err_t *error_out) {
    if (datum.get_type() == ql::datum_t::R_NULL) {
        *value_out = auth_key_t();
        return true;
    } else if (datum.get_type() == ql::datum_t::R_STR) {
        if (!value_out->assign_value(datum.as_str().to_std())) {
            if (datum.as_str().size() > static_cast<size_t>(auth_key_t::max_length)) {
                *error_out = admin_err_t{
                    strprintf("The auth key should be at most %zu bytes long, "
                              "but your given key is %zu bytes.",
                              static_cast<size_t>(auth_key_t::max_length),
                              datum.as_str().size()),
                    query_state_t::FAILED};
            } else {
                /* Currently this can't happen, because length is the only reason to
                invalidate an auth key. This is here for future-proofing. */
                *error_out = admin_err_t{"The given auth key is invalid.",
                                         query_state_t::FAILED};
            }
            return false;
        }
        return true;
    } else if (datum == make_hidden_auth_key_datum()) {
        *error_out = admin_err_t{
            "You're trying to set the `auth_key` field in the `auth` document "
            "of `rethinkdb.cluster_config` to {hidden: true}. The `auth_key` field "
            "can be set to a string, or `null` for no auth key. {hidden: true} is a "
            "special place-holder value that RethinkDB returns if you try to read the "
            "auth key; RethinkDB won't show you the real auth key for security reasons. "
            "Setting the auth key to {hidden: true} is not allowed.",
            query_state_t::FAILED};
        return false;
    } else {
        *error_out = admin_err_t{"Expected a string or null; got " + datum.print(),
                                 query_state_t::FAILED};
        return false;
    }
}

bool cluster_config_artificial_table_backend_t::auth_doc_t::read(
        UNUSED signal_t *interruptor,
        ql::datum_t *row_out,
        UNUSED admin_err_t *error_out) {
    on_thread_t thread_switcher(sl_view->home_thread());
    ql::datum_object_builder_t obj_builder;
    obj_builder.overwrite("id", ql::datum_t("auth"));
    obj_builder.overwrite("auth_key", convert_auth_key_to_datum(
        sl_view->get().auth_key.get_ref()));
    *row_out = std::move(obj_builder).to_datum();
    return true;
}

bool cluster_config_artificial_table_backend_t::auth_doc_t::write(
        UNUSED signal_t *interruptor,
        ql::datum_t *row_inout,
        admin_err_t *error_out) {
    converter_from_datum_object_t converter;
    admin_err_t dummy_error;
    if (!converter.init(*row_inout, &dummy_error)) {
        crash("artificial_table_t should guarantee input is an object");
    }
    ql::datum_t dummy_pkey;
    if (!converter.get("id", &dummy_pkey, &dummy_error)) {
        crash("artificial_table_t should guarantee primary key is present and correct");
    }

    ql::datum_t auth_key_datum;
    if (!converter.get("auth_key", &auth_key_datum, error_out)) {
        return false;
    }
    auth_key_t auth_key;
    if (!convert_auth_key_from_datum(auth_key_datum, &auth_key, error_out)) {
        return false;
    }

    if (!converter.check_no_extra_keys(error_out)) {
        return false;
    }

    {
        on_thread_t thread_switcher(sl_view->home_thread());
        auth_semilattice_metadata_t md = sl_view->get();
        md.auth_key.set(auth_key);
        sl_view->join(md);
    }

    return true;
}

/* There's a weird corner case with changefeeds on the `auth` doc: If the user changes
the authentication key from a non-empty value to another non-empty value, no entry will
appear in the change feed, because the document hasn't changed from the point of view of
the `cfeed_artificial_table_backend_t`. We could work around this by having a way of
forcing the `cfeed_artificial_table_backend_t` to send a change for the row even if it
looks the same, but it's probably not worth the effort. */

void cluster_config_artificial_table_backend_t::auth_doc_t::set_notification_callback(
        const std::function<void()> &fun) {
    if (static_cast<bool>(fun)) {
        subs = make_scoped<semilattice_read_view_t<
            auth_semilattice_metadata_t>::subscription_t>(fun, sl_view);
    } else {
        subs.reset();
    }
}

