'use strict';
'require baseclass';
'require uci';
'require antilag.status as antilagStatus';

/*
 * 75_antilag.js  –  Antilag status widget for the LuCI Overview page.
 * Thin wrapper: all rendering/polling logic lives in the shared
 * antilag.status module (also used by the Antilag settings page).
 */
return baseclass.extend({
    title: _('Antilag'),

    load: function() {
        return uci.load('antilag');
    },

    render: function() {
        return antilagStatus.render(E('div', {}));
    }
});
