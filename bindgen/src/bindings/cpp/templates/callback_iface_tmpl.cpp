{%- let ffi_converter_name = typ|ffi_converter_name %}
{%- let class_name = ffi_converter_name|class_name %}
{%- let canonical_type_name = typ|canonical_name %}
{%- let trait_impl = canonical_type_name|callback_interface_name %}

{%- for (ffi_callback, method) in vtable_methods.iter() %}
 {% call macros::ffi_return_type(ffi_callback) %} {{ trait_impl}}::{{ method.name()|var_name }}({% call macros::arg_list_ffi_decl_xx(ffi_callback) %}) noexcept {
    {%- if method.is_async() %}
    {%- let result_struct = method.foreign_future_ffi_result_struct() %}
    using Result = {{ result_struct.name()|ffi_struct_name }};
    using Complete = void (*)(uint64_t, Result);
    auto complete = reinterpret_cast<Complete>(uniffi_future_callback);
    auto state = std::make_shared<ForeignFutureTaskState>();

    state->set_cancel_completion([complete, uniffi_callback_data] {
        Result result{};
        result.call_status.code = 3;
        complete(uniffi_callback_data, result);
    });
    state->start(state);
    uniffi_out_dropped_callback.handle = foreign_future_handle(state);
    uniffi_out_dropped_callback.free = reinterpret_cast<void *>(&foreign_future_drop);

    auto deliver_failure = [state, complete, uniffi_callback_data](std::exception_ptr error) {
        if (!state->finish()) {
            return;
        }

        Result result{};
        try {
            if (error) {
                std::rethrow_exception(error);
            }
            throw std::runtime_error("UniFFI foreign future failed without an exception");
        }
        {%- match method.throws_type() %}
        {%- when Some(error) %}
        catch ({{ error|canonical_name }} &e) {
            result.call_status.code = 1;
            result.call_status.error_buf = {{ error|lower_fn }}(e);
        }
        {%- when None %}
        {%- endmatch %}
        catch (std::exception &e) {
            result.call_status.code = 2;
            result.call_status.error_buf = {{ Type::String.borrow()|lower_fn }}(e.what());
        } catch (...) {
            result.call_status.code = 2;
            result.call_status.error_buf = {{ Type::String.borrow()|lower_fn }}("Unknown C++ exception");
        }
        complete(uniffi_callback_data, result);
    };

    {% match method.return_type() %}
    {% when Some(t) %}
    auto deliver_success = [state, complete, uniffi_callback_data](const {{ t|type_name(ci) }} &value) {
        if (!state->finish()) {
            return;
        }

        Result result{};
        try {
            result.return_value = {{ t|lower_fn }}(value);
        } catch (std::exception &e) {
            result.call_status.code = 2;
            result.call_status.error_buf = {{ Type::String.borrow()|lower_fn }}(e.what());
        } catch (...) {
            result.call_status.code = 2;
            result.call_status.error_buf = {{ Type::String.borrow()|lower_fn }}("Unknown C++ exception");
        }
        complete(uniffi_callback_data, result);
    };
    {% when None %}
    auto deliver_success = [state, complete, uniffi_callback_data]() {
        if (!state->finish()) {
            return;
        }
        complete(uniffi_callback_data, Result{});
    };
    {% endmatch %}

    auto complete_failure = [state, deliver_failure](std::exception_ptr error) {
        state->enqueue_or_cancel([deliver_failure, error] {
            deliver_failure(error);
        });
    };
    {% match method.return_type() %}
    {% when Some(t) %}
    auto complete_success = [state, deliver_success, complete_failure]({{ t|type_name(ci) }} &&value) {
        try {
            state->enqueue_or_cancel([deliver_success, value = std::move(value)] {
                deliver_success(value);
            });
        } catch (...) {
            complete_failure(std::current_exception());
        }
    };
    {% when None %}
    auto complete_success = [state, deliver_success]() {
        state->enqueue_or_cancel([deliver_success] { deliver_success(); });
    };
    {% endmatch %}

    try {
        auto obj = std::dynamic_pointer_cast<{{ foreign_interface_name }}>(
            {{ ffi_converter_name }}::handle_map.at(uniffi_handle)
        );
        if (!obj) {
            throw std::runtime_error("UniFFI callback handle has the wrong implementation type");
        }
        {%- for arg in method.arguments() %}
        auto arg{{ loop.index0 }} = {{- arg|lift_fn }}({{ arg.name()|var_name }});
        {%- endfor %}
        state->enqueue_or_cancel([
            state, obj, complete_success, complete_failure
            {%- for arg in method.arguments() %}, arg{{ loop.index0 }} = std::move(arg{{ loop.index0 }}){% endfor %}
        ]() mutable {
            if (!::uniffi::detail::begin_async_call() || !state->can_start()) {
                state->cancel();
                return;
            }
            try {
                auto future = obj->{{ method.name()|var_name }}(
                {%- for arg in method.arguments() %}
                    arg{{ loop.index0 }}{%- if !loop.last %}, {% endif %}
                {%- endfor %}
                );
                auto cancel = std::move(future).start(complete_success, complete_failure);
                state->set_cancel([obj, cancel = std::move(cancel)] {
                    if (cancel) {
                        cancel();
                    }
                });
            } catch (...) {
                complete_failure(std::current_exception());
            }
        });
    } catch (...) {
        complete_failure(std::current_exception());
    }
    {%- else %}
    auto obj = {{ ffi_converter_name }}::handle_map.at(uniffi_handle);

    auto make_call = [&]() {% match method.return_type() %}{% when Some(t) %}-> {{ t|type_name(ci) }}{% when None %}{% endmatch %} {
        {%- for arg in method.arguments() %}
        auto arg{{ loop.index0 }} = {{- arg|lift_fn }}({{ arg.name()|var_name }});
        {%- endfor -%}

        {%- if method.return_type().is_some() %}return {% endif -%}
         obj->{{ method.name()|var_name }}(
        {%- for arg in method.arguments() %}
        arg{{ loop.index0 }}{%- if !loop.last %}, {% else %}{% endif %}
        {%- endfor -%}
        );
    };

    {% match method.return_type() %}
    {% when Some(t) %}
    auto write_value = [&]({{ t|type_name(ci) }} v) {
        uniffi_out_return = {{ t|lower_fn }}(v);
    };
    {% when None %}
    (void)uniffi_out_return;
    auto write_value = [](){};
    {% endmatch %}

    {% match method.throws_type() %}
    {% when Some(error) %}
        rust_call_trait_interface_with_error<{{ error|canonical_name }}>(out_status, make_call, write_value, {{ error|lower_fn }});
    {% when None %}
        rust_call_trait_interface(out_status, make_call, write_value);
    {% endmatch %}
    {%- endif %}
}
{%- endfor %}

void {{ trait_impl }}::uniffi_free(uint64_t uniffi_handle) {
    {{ ffi_converter_name }}::handle_map.erase(uniffi_handle);
}

uint64_t {{ trait_impl }}::uniffi_clone(uint64_t uniffi_handle) {
    return {{ ffi_converter_name }}::handle_map.insert(
        {{ ffi_converter_name }}::handle_map.at(uniffi_handle)
    );
}

void {{ trait_impl }}::init() {
    {{ ffi_init_callback.name() }}(vtable);
}
