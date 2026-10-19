import QtQuick

NumberInput {
    textFromValue: function (value, locale) {
        return String(value) + " ms";
    }

    valueFromText: function (text, locale) {
        return parseInt(text, 10);
    }

    validator: RegularExpressionValidator {
        regularExpression: /[0-9]{1,5}(\s*ms)?/
    }
}
