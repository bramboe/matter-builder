import {EditorState} from '@codemirror/state';
import {
  EditorView, drawSelection, highlightActiveLine, highlightActiveLineGutter,
  highlightSpecialChars, keymap, lineNumbers,
} from '@codemirror/view';
import {defaultKeymap, history, historyKeymap, redo, redoDepth, undo, undoDepth} from '@codemirror/commands';
import {
  bracketMatching, codeFolding, foldGutter, foldKeymap, HighlightStyle,
  indentOnInput, syntaxHighlighting,
} from '@codemirror/language';
import {yaml} from '@codemirror/lang-yaml';
import {highlightSelectionMatches, openSearchPanel, search, searchKeymap} from '@codemirror/search';
import {tags} from '@lezer/highlight';
import {structuredPatch} from 'diff';

const colors = HighlightStyle.define([
  {tag: tags.propertyName, color: 'var(--yaml-key)'},
  {tag: [tags.string, tags.special(tags.string)], color: 'var(--yaml-string)'},
  {tag: [tags.number, tags.bool, tags.null], color: 'var(--yaml-value)'},
  {tag: tags.comment, color: 'var(--mute)', fontStyle: 'italic'},
  {tag: [tags.labelName, tags.atom, tags.typeName], color: 'var(--yaml-value)'},
]);

function renderPreview(panel, original, current) {
  panel.replaceChildren();
  original = original.replace(/\r\n?/g, '\n');
  if (original === current) {
    panel.textContent = 'No YAML changes to preview.';
    return;
  }
  const patch = structuredPatch('', '', original, current, undefined, undefined, {context: 3, timeout: 100});
  if (!patch) {
    panel.textContent = 'This change is too large to preview. Return to Edit to review the YAML.';
    return;
  }
  const diff = document.createElement('pre');
  for (const hunk of patch.hunks) {
    const heading = document.createElement('span');
    heading.className = 'yaml-diff-range';
    heading.textContent = `@@ -${hunk.oldStart},${hunk.oldLines} +${hunk.newStart},${hunk.newLines} @@
`;
    diff.append(heading);
    for (const line of hunk.lines) {
      const row = document.createElement('span');
      if (line.startsWith('+')) row.className = 'yaml-diff-add';
      if (line.startsWith('-')) row.className = 'yaml-diff-remove';
      row.textContent = line + '\n';
      diff.append(row);
    }
  }
  panel.append(diff);
}

class YamlEditor extends HTMLElement {
  setOriginal(text) {
    this.dataset.original = text;
    const preview = this.querySelector('.yaml-preview:not([hidden])');
    if (preview) renderPreview(preview, text, this.querySelector('textarea').value);
  }

  connectedCallback() {
    if (this.view) return;
    const input = this.querySelector('textarea');
    if (!input) return;
    const toolbar = document.createElement('div');
    toolbar.className = 'yaml-tools';
    toolbar.setAttribute('role', 'group');
    toolbar.setAttribute('aria-label', 'YAML editor tools');

    const button = (label, action, group) => {
      const control = document.createElement('button');
      control.type = 'button';
      control.className = 'plain';
      control.textContent = label;
      control.addEventListener('click', () => action(this.view));
      group.append(control);
      return control;
    };
    const editor = document.createElement('div');
    editor.className = 'yaml-code';
    const tools = document.createElement('div');
    tools.className = 'yaml-actions';
    if (!input.readOnly) {
      const preview = document.createElement('div');
      preview.className = 'yaml-preview';
      preview.setAttribute('role', 'region');
      preview.setAttribute('aria-label', 'YAML changes');
      preview.tabIndex = 0;
      const switchMode = showPreview => {
        editor.hidden = tools.hidden = showPreview;
        preview.hidden = !showPreview;
        edit.setAttribute('aria-pressed', String(!showPreview));
        review.setAttribute('aria-pressed', String(showPreview));
        if (showPreview) {
          renderPreview(preview, this.dataset.original ?? input.defaultValue, input.value);
          preview.focus();
        } else this.view.focus();
      };
      const modes = document.createElement('div');
      modes.className = 'yaml-modes';
      const edit = button('Edit', () => switchMode(false), modes);
      const review = button('Preview', () => switchMode(true), modes);
      edit.setAttribute('aria-pressed', 'true');
      review.setAttribute('aria-pressed', 'false');
      preview.hidden = true;
      toolbar.append(modes);
      this.append(preview);
    }
    toolbar.append(tools);
    button('Find', openSearchPanel, tools);
    const undoButton = input.readOnly ? null : button('Undo', view => { undo(view); view.focus(); }, tools);
    const redoButton = input.readOnly ? null : button('Redo', view => { redo(view); view.focus(); }, tools);
    const updateHistory = state => {
      if (!undoButton) return;
      undoButton.disabled = undoDepth(state) === 0;
      redoButton.disabled = redoDepth(state) === 0;
    };
    this.prepend(toolbar);
    this.append(editor);
    this.view = new EditorView({
      doc: input.value,
      parent: editor,
      extensions: [
        lineNumbers(), foldGutter(), codeFolding(), highlightSpecialChars(),
        drawSelection(), highlightActiveLine(), highlightActiveLineGutter(),
        search({top: true}), highlightSelectionMatches(),
        yaml(), indentOnInput(), bracketMatching(), syntaxHighlighting(colors),
        EditorState.readOnly.of(input.readOnly),
        EditorView.editable.of(!input.readOnly),
        EditorView.contentAttributes.of({'aria-label': 'YAML', spellcheck: 'false', tabindex: '0'}),
        ...(input.readOnly ? [] : [history()]),
        keymap.of([...searchKeymap, ...historyKeymap, ...foldKeymap, ...defaultKeymap]),
        EditorView.updateListener.of(update => {
          if (update.docChanged) {
            input.value = update.state.doc.toString();
            input.dispatchEvent(new Event('input', {bubbles: true}));
          }
          updateHistory(update.state);
        }),
      ],
    });
    updateHistory(this.view.state);
    input.hidden = true;
  }

  disconnectedCallback() {
    if (!this.view) return;
    this.view.destroy();
    this.view = null;
    this.querySelector('.yaml-tools').remove();
    this.querySelector('.yaml-code').remove();
    this.querySelector('.yaml-preview')?.remove();
    this.querySelector('textarea').hidden = false;
  }
}

customElements.define('mb-yaml-editor', YamlEditor);
