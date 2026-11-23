{% macro rust_call(func) -%}
    uniffi::rust_call(
        {{ func.ffi_func().name() }},
{%- match func.throws_type() %}
{% when Some with (e) %}
        uniffi::{{ e|ffi_error_converter_name }}::lift
{%- else %}
        nullptr
{%- endmatch %}
{%- if !func.arguments().is_empty() %}, {% else %}{% endif %}
        {%- call arg_list_lowered(func) -%})
{%- endmacro %}

{% macro rust_call_with_prefix(prefix, func) -%}
    uniffi::rust_call(
        {{ func.ffi_func().name() }},
{%- match func.throws_type() %}
{% when Some with (e) %}
        uniffi::{{ e|ffi_error_converter_name }}::lift,
{%- else %}
        nullptr,
{%- endmatch %}
        {{ prefix }}
{%- if !func.arguments().is_empty() %}, {% else %}{% endif %}
        {%- call arg_list_lowered(func) -%})
{%- endmacro %}

{% macro rust_call_with_self(func, self_type) -%}
    uniffi::rust_call(
        {{ func.ffi_func().name() }},
{%- match func.throws_type() %}
{% when Some with (e) %}
        uniffi::{{ e|ffi_error_converter_name }}::lift,
{%- else %}
        nullptr,
{%- endmatch %}
        uniffi::{{ self_type|lower_fn }}(*this)
{%- if !func.arguments().is_empty() %}, {% else %}{% endif %}
        {%- call arg_list_lowered(func) -%})
{%- endmacro %}

{% macro rust_call_with_value(func, self_type) -%}
    uniffi::rust_call(
        {{ func.ffi_func().name() }},
{%- match func.throws_type() %}
{% when Some with (e) %}
        uniffi::{{ e|ffi_error_converter_name }}::lift,
{%- else %}
        nullptr,
{%- endmatch %}
        uniffi::{{ self_type|lower_fn }}(_uniffi_self)
{%- if !func.arguments().is_empty() %}, {% else %}{% endif %}
        {%- call arg_list_lowered(func) -%})
{%- endmacro %}

{% macro rust_call_with_self_and_other(func, self_type) -%}
    uniffi::rust_call(
        {{ func.ffi_func().name() }},
{%- match func.throws_type() %}
{% when Some with (e) %}
        uniffi::{{ e|ffi_error_converter_name }}::lift,
{%- else %}
        nullptr,
{%- endmatch %}
        uniffi::{{ self_type|lower_fn }}(*this),
        uniffi::{{ self_type|lower_fn }}(other))
{%- endmacro %}

{% macro rust_call_async(func, prefix) -%}
    uniffi::rust_call_async<{% match func.return_type() %}{% when Some with (value_type) %}{{ value_type|type_name(ci) }}{% when None %}void{% endmatch %}>(
        [&]() {
            return {{ func.ffi_func().name() }}(
                {%- if !prefix.to_string().is_empty() -%}
                {{ prefix }}{% if !func.arguments().is_empty() %}, {% endif %}
                {%- endif -%}
                {%- call arg_list_lowered(func) -%}
            );
        },
        {{ func.ffi_rust_future_poll(ci) }},
        {{ func.ffi_rust_future_cancel(ci) }},
        {{ func.ffi_rust_future_complete(ci) }},
        {{ func.ffi_rust_future_free(ci) }},
{%- match func.return_type() %}
{% when Some with (value_type) %}
        [](auto value) { return uniffi::{{ value_type|lift_fn }}(value); },
{% when None %}
        nullptr,
{%- endmatch %}
{%- match func.throws_type() %}
{% when Some with (e) %}
        uniffi::{{ e|ffi_error_converter_name }}::lift
{%- else %}
        nullptr
{%- endmatch %}
    )
{%- endmacro %}

{% macro rust_call_async_with_value(func, self_type, receiver) -%}
    {%- let converter = self_type|lower_fn -%}
    {%- let prefix = "uniffi::" ~ converter ~ "(" ~ receiver ~ ")" -%}
    {%- call rust_call_async(func, prefix) -%}
{%- endmacro %}

{% macro callable_return_type(method) -%}
    {%- if method.is_async() %}::uniffi::Future<{% endif -%}
    {%- match method.return_type() -%}
    {%- when Some with (return_type) -%}{{ return_type|type_name(ci) }}
    {%- when None -%}void
    {%- endmatch -%}
    {%- if method.is_async() %}>{% endif -%}
{%- endmacro %}

{% macro param_list(func) %}
{%- for arg in func.arguments() -%}
{{ arg|parameter(ci) }}
{%- if !loop.last -%}, {% endif -%}
{% endfor -%}
{% endmacro %}

{% macro field_name(field, field_num) %}
{%- if field.name().is_empty() -%}
v{{- field_num -}}
{%- else -%}
{{ field.name()|var_name }}
{%- endif -%}
{%- endmacro %}

{% macro arg_list_lowered(func) %}
{%- for arg in func.arguments() -%}
uniffi::{{ arg|lower_fn }}({{ arg.as_type()|cpp_deref(ci) }}{{ arg.name()|var_name }})
{%- if !loop.last -%}, {% endif -%}
{% endfor -%}
{% endmacro %}

{%- macro arg_list_ffi_decl_xx(func) %}
    {%- for arg in func.arguments() %}
        {{- arg.type_().borrow()|ffi_type_name }} {{ arg.name()|var_name -}}{%- if !loop.last || func.has_rust_call_status_arg() -%},{%- endif -%}
    {%- endfor %}
    {%- if func.has_rust_call_status_arg() %}RustCallStatus *out_status{% endif %}
{%- endmacro -%}

{%- macro ffi_return_type(func) %}
    {%- match func.return_type() %}
    {%- when Some(return_type) %}{{ return_type|ffi_type_name }}
    {%- when None %}{{ "void" }}
    {%- endmatch %}
{%- endmacro %}

{%- macro docstring_value(maybe_docstring, indent_spaces) %}
{%- match maybe_docstring %}
{%- when Some(docstring) %}
{{ docstring|docstring(indent_spaces) }}
{%- else %}
{%- endmatch %}
{%- endmacro %}

{%- macro docstring(defn, indent_spaces) %}
{%- call docstring_value(defn.docstring(), indent_spaces) %}
{%- endmacro %}
