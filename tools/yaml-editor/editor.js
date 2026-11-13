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

const colors = HighlightStyle.define([
  {tag: tags.propertyName, color: 'var(--yaml-key)'},
  {tag: [tags.string, tags.special(tags.string)], color: 'var(--yaml-string)'},
  {tag: [tags.number, tags.bool, tags.null], color: 'var(--yaml-value)'},
  {tag: tags.comment, color: 'var(--mute)', fontStyle: 'italic'},
  {tag: [tags.labelName, tags.atom, tags.typeName], color: 'var(--yaml-value)'},
]);

class YamlEditor extends HTMLElement {
  connectedCallback() {
    if (this.view) return;
    const input = this.querySelector('textarea');
    if (!input) return;
    const toolbar = document.createElement('div');
    toolbar.className = 'yaml-tools';
    toolbar.setAttribute('role', 'group');
    toolbar.setAttribute('aria-label', 'YAML editor tools');

    const button = (label, action) => {
      const control = document.createElement('button');
      control.type = 'button';
      control.className = 'plain';
      control.textContent = label;
      control.addEventListener('click', () => action(this.view));
      toolbar.append(control);
      return control;
    };
    button('Find', openSearchPanel);
    const undoButton = input.readOnly ? null : button('Undo', view => { undo(view); view.focus(); });
    const redoButton = input.readOnly ? null : button('Redo', view => { redo(view); view.focus(); });
    const updateHistory = state => {
      if (!undoButton) return;
      undoButton.disabled = undoDepth(state) === 0;
      redoButton.disabled = redoDepth(state) === 0;
    };
    const editor = document.createElement('div');
    editor.className = 'yaml-code';
    this.append(toolbar, editor);
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
    this.querySelector('textarea').hidden = false;
  }
}

customElements.define('mb-yaml-editor', YamlEditor);
