"""The '@' file picker in the composer."""


def tree(ctx):
    ctx.write_file("notes.txt", "hello\n")
    ctx.write_file("readme.md", "docs\n")
    ctx.write_file("src/main.c", "int main(void){}\n")
    ctx.write_file("src/util.c", "void u(void){}\n")


def test_at_lists_the_working_directory(ctx):
    """'@' offers what is here, directories first and marked with a slash."""
    tree(ctx)
    s = ctx.spawn()
    s.type("@").sync()
    text = s.text()
    assert "src/" in text, text
    assert "notes.txt" in text and "readme.md" in text, text
    rows = [row for row in s.screen.lines() if "src/" in row or "notes.txt" in row]
    assert "src/" in rows[0], rows


def test_typing_narrows_the_list(ctx):
    """The word after '@' filters the entries by prefix."""
    tree(ctx)
    s = ctx.spawn()
    s.type("@no").sync()
    text = s.text()
    assert "notes.txt" in text, text
    assert "readme.md" not in text, text


def test_tab_accepts_a_file(ctx):
    """Tab writes the path into the composer, '@' kept as the marker."""
    tree(ctx)
    s = ctx.spawn()
    s.type("read @no").sync()
    s.key("tab").sync()
    assert s.composer_text() == "read @notes.txt", s.composer_lines()
    assert "readme.md" not in s.text(), "popup should be closed"


def test_a_directory_is_a_step_not_an_answer(ctx):
    """Picking a folder lists what is in it and never submits."""
    tree(ctx)
    s = ctx.spawn()
    s.type("@sr").sync()
    s.key("enter").sync()
    assert s.composer_text() == "@src/", s.composer_lines()
    text = s.text()
    assert "src/main.c" in text and "src/util.c" in text, text
    assert s.proc.poll() is None
    s.key("enter").sync()
    assert s.composer_text() == "@src/main.c", s.composer_lines()
    assert s.proc.poll() is None, "accepting a path must not send the message"


def test_at_inside_a_word_is_just_text(ctx):
    """A '@' that does not start the word is an address, not a picker."""
    tree(ctx)
    s = ctx.spawn()
    s.type("mail me@no").sync()
    assert "notes.txt" not in s.text(), s.text()


def test_the_picked_path_is_sent_with_the_message(ctx):
    """The composed line is a message like any other."""
    tree(ctx)
    ctx.scenario("text=on+it")
    s = ctx.spawn()
    s.type("read @no").sync()
    s.key("tab").sync()
    s.key("enter")
    s.wait_text("read @notes.txt")
    s.wait_turn_done()


def test_arrows_move_the_path_selection(ctx):
    """Down highlights the next entry, and typing does not undo the move."""
    tree(ctx)
    s = ctx.spawn()
    s.type("@").sync()
    s.key("down").sync()          # off 'src/' and onto the first file
    rows = [row for row in s.screen.lines() if "\u203a " in row]
    assert rows and "notes.txt" in rows[0], s.screen.lines()
    s.key("tab").sync()
    assert s.composer_text() == "@notes.txt", s.composer_lines()


def test_a_moved_selection_survives_typing(ctx):
    """Narrowing the list keeps the highlight on the entry it was on."""
    ctx.write_file("notes.txt", "a\n")
    ctx.write_file("notebook.md", "b\n")
    s = ctx.spawn()
    s.type("@note").sync()
    s.key("down").sync()          # 'notes.txt', the second of the two
    s.type("s").sync()
    s.key("tab").sync()
    assert s.composer_text() == "@notes.txt", s.composer_lines()


def test_gitignore_hides_what_it_excludes(ctx):
    """The project says what is not part of the work; the picker believes it."""
    ctx.write_file(".gitignore", "build/\n*.log\n")
    ctx.write_file("keep.txt", "a\n")
    ctx.write_file("noise.log", "b\n")
    ctx.write_file("build/out.o", "c\n")
    s = ctx.spawn()
    s.type("@").sync()
    text = s.text()
    assert "keep.txt" in text, text
    assert "noise.log" not in text, text
    assert "build/" not in text, text


def test_dot_ignore_is_read_the_same_way(ctx):
    """.ignore says the same thing as .gitignore, so it is read with it."""
    ctx.write_file(".ignore", "secret.txt\n")
    ctx.write_file("secret.txt", "a\n")
    ctx.write_file("public.txt", "b\n")
    s = ctx.spawn()
    s.type("@").sync()
    text = s.text()
    assert "public.txt" in text, text
    assert "secret.txt" not in text, text


def test_a_negation_brings_a_path_back(ctx):
    """The last pattern that matches decides, '!' included."""
    ctx.write_file(".gitignore", "*.log\n!keep.log\n")
    ctx.write_file("drop.log", "a\n")
    ctx.write_file("keep.log", "b\n")
    s = ctx.spawn()
    s.type("@").sync()
    text = s.text()
    assert "keep.log" in text, text
    assert "drop.log" not in text, text


def test_a_nested_ignore_file_applies_to_its_own_directory(ctx):
    """A subdirectory refines its parent rather than replacing it."""
    ctx.write_file(".gitignore", "*.o\n")
    ctx.write_file("src/.gitignore", "scratch.c\n")
    ctx.write_file("src/main.c", "a\n")
    ctx.write_file("src/scratch.c", "b\n")
    ctx.write_file("src/main.o", "c\n")
    s = ctx.spawn()
    s.type("@src/").sync()
    text = s.text()
    assert "src/main.c" in text, text
    assert "src/scratch.c" not in text, text
    assert "src/main.o" not in text, text


def test_the_git_directory_is_never_offered(ctx):
    """Nothing in a repository's bookkeeping is worth mentioning to a model."""
    ctx.write_file(".git/config", "[core]\n")
    ctx.write_file("keep.txt", "a\n")
    s = ctx.spawn()
    s.type("@").sync()
    text = s.text()
    assert "keep.txt" in text, text
    assert ".git/" not in text, text


def test_the_setting_offers_the_ignored_paths(ctx):
    """'Ignored files' lifts the default, and .git stays hidden regardless."""
    ctx.write_file(".gitignore", "*.log\n")
    ctx.write_file(".git/config", "[core]\n")
    ctx.write_file("noise.log", "a\n")
    s = ctx.spawn()
    s.settings_toggle("Ignored files")
    s.type("@").sync()
    text = s.text()
    assert "noise.log" in text, text
    assert ".git/" not in text, text


def test_a_nested_file_is_found_by_name_alone(ctx):
    """A word after '@' searches below the directory, not only inside it."""
    ctx.write_file("src/deep/nested/widget.c", "a\n")
    ctx.write_file("other.txt", "b\n")
    s = ctx.spawn()
    s.type("@widget").sync()
    assert "src/deep/nested/widget.c" in s.text(), s.text()
    s.key("tab").sync()
    assert s.composer_text() == "@src/deep/nested/widget.c", s.composer_lines()


def test_letters_in_order_are_enough(ctx):
    """The letters of the word need only appear in order along the path."""
    ctx.write_file("src/main.c", "a\n")
    ctx.write_file("docs/readme.md", "b\n")
    s = ctx.spawn()
    s.type("@srmain").sync()
    assert "src/main.c" in s.text(), s.text()
    assert "docs/readme.md" not in s.text(), s.text()


def test_a_name_that_starts_with_it_comes_first(ctx):
    """Rank beats depth: a prefix of the name outranks a scattered match."""
    ctx.write_file("deep/dir/notes.txt", "a\n")
    ctx.write_file("noise/other/text.md", "b\n")
    s = ctx.spawn()
    s.type("@not").sync()
    selected = [row for row in s.screen.lines() if "\u203a " in row]
    assert "deep/dir/notes.txt" in selected[0], s.screen.lines()
    assert "noise/other/text.md" in s.text(), s.text()


def test_a_search_below_a_directory_stays_below_it(ctx):
    """'@src/' scopes the search: what is elsewhere is not an answer."""
    ctx.write_file("src/main.c", "a\n")
    ctx.write_file("vendor/main.c", "b\n")
    s = ctx.spawn()
    s.type("@src/main").sync()
    text = s.text()
    assert "src/main.c" in text, text
    assert "vendor/main.c" not in text, text


def test_an_ignored_directory_is_not_searched(ctx):
    """What the project excludes is not walked, so nothing in it turns up."""
    ctx.write_file(".gitignore", "node_modules/\n")
    ctx.write_file("node_modules/pkg/widget.js", "a\n")
    ctx.write_file("app/widget.js", "b\n")
    s = ctx.spawn()
    s.type("@widget").sync()
    text = s.text()
    assert "app/widget.js" in text, text
    assert "node_modules" not in text, text
