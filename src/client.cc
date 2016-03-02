#include "client.hh"

#include "face_registry.hh"
#include "context.hh"
#include "buffer_manager.hh"
#include "buffer_utils.hh"
#include "file.hh"
#include "remote.hh"
#include "client_manager.hh"
#include "command_manager.hh"
#include "event_manager.hh"
#include "user_interface.hh"
#include "window.hh"

#include <signal.h>
#include <unistd.h>

namespace Kakoune
{

Client::Client(std::unique_ptr<UserInterface>&& ui,
               std::unique_ptr<Window>&& window,
               SelectionList selections,
               EnvVarMap env_vars,
               String name)
    : m_ui{std::move(ui)}, m_window{std::move(window)},
      m_input_handler{std::move(selections), Context::Flags::None,
                      std::move(name)},
      m_env_vars(env_vars)
{
    context().set_client(*this);
    context().set_window(*m_window);

    m_window->set_dimensions(m_ui->dimensions());
    m_window->options().register_watcher(*this);

    m_ui->set_ui_options(m_window->options()["ui_options"].get<UserInterface::Options>());
    m_ui->set_input_callback([this](EventMode mode) { handle_available_input(mode); });
}

Client::~Client()
{
    m_window->options().unregister_watcher(*this);
}

Optional<Key> Client::get_next_key(EventMode mode)
{
    if (not m_pending_keys.empty())
    {
        Key key = m_pending_keys.front();
        m_pending_keys.erase(m_pending_keys.begin());
        return key;
    }
    if (mode != EventMode::Pending and m_ui->is_key_available())
        return m_ui->get_key();
    return {};
}

void Client::handle_available_input(EventMode mode)
{
    if (mode == EventMode::Urgent)
    {
        Key key = m_ui->get_key();
        if (key == ctrl('c'))
            killpg(getpgrp(), SIGINT);
        else
            m_pending_keys.push_back(key);
        return;
    }

    try
    {
        try
        {
            while (Optional<Key> key = get_next_key(mode))
            {
                if (*key == ctrl('c'))
                    killpg(getpgrp(), SIGINT);
                else if (*key == Key::FocusIn)
                    context().hooks().run_hook("FocusIn", context().name(), context());
                else if (*key == Key::FocusOut)
                    context().hooks().run_hook("FocusOut", context().name(), context());
                else if (key->modifiers == Key::Modifiers::Resize)
                    force_redraw();
                else
                    m_input_handler.handle_key(*key);
            }
        }
        catch (Kakoune::runtime_error& error)
        {
            context().print_status({ error.what().str(), get_face("Error") });
            context().hooks().run_hook("RuntimeError", error.what(), context());
        }
    }
    catch (Kakoune::client_removed& removed)
    {
        ClientManager::instance().remove_client(*this, removed.graceful);
    }
}

void Client::print_status(DisplayLine status_line)
{
    m_pending_status_line = std::move(status_line);
}

DisplayLine Client::generate_mode_line() const
{
    DisplayLine modeline;
    try
    {
        const String& modelinefmt = context().options()["modelinefmt"].get<String>();

        modeline = parse_display_line(expand(modelinefmt, context()));
    }
    catch (runtime_error& err)
    {
        write_to_debug_buffer(format("Error while parsing modelinefmt: {}", err.what()));
        modeline.push_back({ "modelinefmt error, see *debug* buffer", get_face("Error") });
    }

    Face info_face = get_face("Information");

    if (context().buffer().is_modified())
        modeline.push_back({ "[+]", info_face });
    if (m_input_handler.is_recording())
        modeline.push_back({ format("[recording ({})]", m_input_handler.recording_reg()), info_face });
    if (context().buffer().flags() & Buffer::Flags::New)
        modeline.push_back({ "[new file]", info_face });
    if (context().user_hooks_disabled())
        modeline.push_back({ "[no-hooks]", info_face });
    if (context().buffer().flags() & Buffer::Flags::Fifo)
        modeline.push_back({ "[fifo]", info_face });
    modeline.push_back({ " " });
    for (auto& atom : m_input_handler.mode_line())
        modeline.push_back(std::move(atom));
    modeline.push_back({ format(" - {}@[{}]", context().name(), Server::instance().session()) });

    return modeline;
}

void Client::change_buffer(Buffer& buffer)
{
    if (m_buffer_reload_dialog_opened)
        close_buffer_reload_dialog();

    m_last_buffer = &m_window->buffer();

    auto& client_manager = ClientManager::instance();
    m_window->options().unregister_watcher(*this);
    client_manager.add_free_window(std::move(m_window),
                                   std::move(context().selections()));
    WindowAndSelections ws = client_manager.get_free_window(buffer);

    m_window = std::move(ws.window);
    m_window->options().register_watcher(*this);
    m_ui->set_ui_options(m_window->options()["ui_options"].get<UserInterface::Options>());

    context().selections_write_only() = std::move(ws.selections);
    context().set_window(*m_window);
    m_window->set_dimensions(m_ui->dimensions());

    m_window->hooks().run_hook("WinDisplay", buffer.name(), context());
}

static bool is_inline(InfoStyle style)
{
    return style == InfoStyle::Inline or
           style == InfoStyle::InlineAbove or
           style == InfoStyle::InlineBelow;
}

void Client::redraw_ifn()
{
    Window& window = context().window();

    const bool needs_redraw = window.needs_redraw(context());
    if (needs_redraw)
    {
        auto window_pos = window.position();
        m_ui->draw(window.update_display_buffer(context()), get_face("Default"));

	// window moved, reanchor eventual menu and info
        if (window_pos != window.position())
        {
            if (not m_menu.items.empty() and m_menu.style == MenuStyle::Inline)
            {
                m_ui->menu_show(m_menu.items, window.display_position(m_menu.anchor),
                                get_face("MenuForeground"), get_face("MenuBackground"), m_menu.style);
                m_ui->menu_select(m_menu.selected);
            }
            if (not m_info.content.empty() and is_inline(m_info.style))
                m_ui->info_show(m_info.title, m_info.content,
                                window.display_position(m_info.anchor),
                                get_face("Information"), m_info.style);
        }
    }

    DisplayLine mode_line = generate_mode_line();
    if (needs_redraw or
        m_status_line.atoms() != m_pending_status_line.atoms() or
        mode_line.atoms() != m_mode_line.atoms())
    {
        m_mode_line = std::move(mode_line);
        m_status_line = m_pending_status_line;

        m_ui->draw_status(m_status_line, m_mode_line, get_face("StatusLine"));
    }

    m_ui->refresh();
}

void Client::force_redraw()
{
    if (m_window)
        m_window->force_redraw();
}

void Client::reload_buffer()
{
    Buffer& buffer = context().buffer();
    reload_file_buffer(buffer);
    context().print_status({ format("'{}' reloaded", buffer.display_name()),
                             get_face("Information") });
}

void Client::on_buffer_reload_key(Key key)
{
    auto& buffer = context().buffer();

    if (key == 'y' or key == ctrl('m'))
        reload_buffer();
    else if (key == 'n' or key == Key::Escape)
    {
        // reread timestamp in case the file was modified again
        buffer.set_fs_timestamp(get_fs_timestamp(buffer.name()));
        print_status({ format("'{}' kept", buffer.display_name()),
                       get_face("Information") });
    }
    else
    {
        print_status({ format("'{}' is not a valid choice", key_to_str(key)),
                       get_face("Error") });
        m_input_handler.on_next_key(KeymapMode::None, [this](Key key, Context&){ on_buffer_reload_key(key); });
        return;
    }

    for (auto& client : ClientManager::instance())
    {
        if (&client->context().buffer() == &buffer and
            client->m_buffer_reload_dialog_opened)
            client->close_buffer_reload_dialog();
    }
}

void Client::close_buffer_reload_dialog()
{
    kak_assert(m_buffer_reload_dialog_opened);
    m_buffer_reload_dialog_opened = false;
    m_ui->info_hide();
    m_input_handler.reset_normal_mode();
}

void Client::check_if_buffer_needs_reloading()
{
    if (m_buffer_reload_dialog_opened)
        return;

    Buffer& buffer = context().buffer();
    auto reload = context().options()["autoreload"].get<Autoreload>();
    if (not (buffer.flags() & Buffer::Flags::File) or reload == Autoreload::No)
        return;

    const String& filename = buffer.name();
    timespec ts = get_fs_timestamp(filename);
    if (ts == InvalidTime or ts == buffer.fs_timestamp())
        return;
    if (reload == Autoreload::Ask)
    {
        StringView bufname = buffer.display_name();
        info_show(format("reload '{}' ?", bufname),
                  format("'{}' was modified externally\n"
                         "press <ret> or y to reload, <esc> or n to keep",
                         bufname), {}, InfoStyle::Prompt);

        m_buffer_reload_dialog_opened = true;
        m_input_handler.on_next_key(KeymapMode::None, [this](Key key, Context&){ on_buffer_reload_key(key); });
    }
    else
        reload_buffer();
}

StringView Client::get_env_var(StringView name) const
{
    auto it = m_env_vars.find(name);
    if (it == m_env_vars.end())
        return {};
    return it->value;
}

void Client::on_option_changed(const Option& option)
{
    if (option.name() == "ui_options")
        m_ui->set_ui_options(option.get<UserInterface::Options>());
}

void Client::menu_show(Vector<DisplayLine> choices, ByteCoord anchor, MenuStyle style)
{
    m_menu = Menu{ std::move(choices), anchor, style, -1 };
    CharCoord ui_anchor = style == MenuStyle::Inline ? context().window().display_position(anchor) : CharCoord{};
    m_ui->menu_show(m_menu.items, ui_anchor, get_face("MenuForeground"), get_face("MenuBackground"), style);
}

void Client::menu_select(int selected)
{
    m_menu.selected = selected;
    m_ui->menu_select(selected);
}

void Client::menu_hide()
{
    m_menu = Menu{};
    m_ui->menu_hide();
}

void Client::info_show(String title, String content, ByteCoord anchor, InfoStyle style)
{
    m_info = Info{ std::move(title), std::move(content), anchor, style };
    CharCoord ui_anchor = is_inline(style) ? context().window().display_position(anchor) : CharCoord{};
    m_ui->info_show(m_info.title, m_info.content, ui_anchor, get_face("Information"), style);
}

void Client::info_hide()
{
    m_info = Info{};
    m_ui->info_hide();
}

}
