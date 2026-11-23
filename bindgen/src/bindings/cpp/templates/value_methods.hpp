{%- for method in methods %}
{%- call macros::docstring(method, 4) %}
{% call macros::callable_return_type(method) %} {{ method.name()|fn_name }}({% call macros::param_list(method) %}) const;
{%- endfor %}

{%- if uniffi_trait_methods.display_fmt.is_some() %}
    /**
     * Returns a string representation of the value, internally calls Rust's `Display` trait.
     */
    std::string to_string() const;
{%- endif %}
{%- if uniffi_trait_methods.debug_fmt.is_some() %}
    /**
     * Returns a string representation of the value, internally calls Rust's `Debug` trait.
     */
    std::string to_debug_string() const;
{%- endif %}
{%- if uniffi_trait_methods.eq_eq.is_some() %}
    /**
     * Equality check, internally calls Rust's `Eq` trait.
     */
    bool eq(const {{ type_name }} &other) const;
    /**
     * Inequality check, internally calls Rust's `Ne` trait.
     */
    bool ne(const {{ type_name }} &other) const;
{%- endif %}
{%- if uniffi_trait_methods.hash_hash.is_some() %}
    /**
     * Returns a hash of the value, internally calls Rust's `Hash` trait.
     */
    uint64_t hash() const;
{%- endif %}
{%- if uniffi_trait_methods.ord_cmp.is_some() %}
    /**
     * Three-way comparison, internally calls Rust's `Ord` trait.
     */
    int8_t cmp(const {{ type_name }} &other) const;
{%- endif %}
