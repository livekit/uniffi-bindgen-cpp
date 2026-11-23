{% call macros::callable_return_type(func) %} {{ func.name()|fn_name }}({% call macros::param_list(func) %}) {
    {%- if func.is_async() %}
    return {% call macros::rust_call_async(func, "") %};
    {%- else %}
    {%- match func.return_type() %}
    {%- when Some with (return_type) %}
    auto ret = {% call macros::rust_call(func) %};

    return uniffi::{{ return_type|lift_fn }}(ret);
    {%- when None %}
    {% call macros::rust_call(func) %};
    {%- endmatch %}
    {%- endif %}
}
