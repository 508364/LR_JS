function t(name, fn) {
    try { console.log(name + ": " + fn()); }
    catch (e) { console.log(name + ": THROW " + e); }
}
t("(\\d)", () => /(\d)/.source);
t("(a{4})", () => /(a{4})/.source);
t("(\\d4)", () => /(\d4)/.source);
t("(\\d{4})", () => /(\d{4})/.source);
t("\\d{4}", () => /\d{4}/.source);
t("ctor same", () => new RegExp("(\\d{4})").source);
