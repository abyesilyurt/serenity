/*
 * Copyright (c) 2020, Alex McGrath <amk@amk.ie>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGUI/Action.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/Event.h>
#include <LibGUI/LinkLabel.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font.h>
#include <LibGfx/Palette.h>

REGISTER_WIDGET(GUI, LinkLabel)

namespace GUI {

LinkLabel::LinkLabel(String text)
    : Label(move(text))
{
    set_override_cursor(Gfx::StandardCursor::Hand);
    set_foreground_role(Gfx::ColorRole::Link);
    set_focus_policy(FocusPolicy::TabFocus);
    setup_actions();
}

void LinkLabel::setup_actions()
{
    m_open_action = GUI::Action::create("Show in File Manager", {}, Gfx::Bitmap::try_load_from_file("/res/icons/16x16/app-file-manager.png"), [&](const GUI::Action&) {
        if (on_click)
            on_click();
    });

    m_copy_action = CommonActions::make_copy_action([this](auto&) { Clipboard::the().set_plain_text(text()); }, this);
}

void LinkLabel::mousedown_event(MouseEvent& event)
{
    if (event.button() != MouseButton::Left)
        return;

    Label::mousedown_event(event);
    if (on_click) {
        on_click();
    }
}

void LinkLabel::keydown_event(KeyEvent& event)
{
    Label::keydown_event(event);
    if (event.key() == KeyCode::Key_Return || event.key() == KeyCode::Key_Space) {
        if (on_click)
            on_click();
    }
}

void LinkLabel::paint_event(PaintEvent& event)
{
    Label::paint_event(event);
    GUI::Painter painter(*this);

    if (m_hovered)
        painter.draw_line({ 0, rect().bottom() }, { font().width(text()), rect().bottom() }, palette().link());

    if (is_focused())
        painter.draw_focus_rect(text_rect(), palette().focus_outline());
}

void LinkLabel::enter_event(Core::Event& event)
{
    Label::enter_event(event);
    m_hovered = true;
    update();
}

void LinkLabel::leave_event(Core::Event& event)
{
    Label::leave_event(event);
    m_hovered = false;
    update();
}

void LinkLabel::did_change_text()
{
    Label::did_change_text();
    update_tooltip_if_needed();
}

void LinkLabel::update_tooltip_if_needed()
{
    if (width() < font().width(text())) {
        set_tooltip(text());
    } else {
        set_tooltip({});
    }
}

void LinkLabel::resize_event(ResizeEvent& event)
{
    Label::resize_event(event);
    update_tooltip_if_needed();
}

void LinkLabel::context_menu_event(ContextMenuEvent& event)
{
    if (!m_context_menu) {
        m_context_menu = Menu::construct();
        m_context_menu->add_action(*m_open_action);
        m_context_menu->add_separator();
        m_context_menu->add_action(*m_copy_action);
    }
    m_context_menu->popup(event.screen_position(), m_open_action);
}

}
