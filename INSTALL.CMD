/* Create or update the Craft Suite Workplace Shell launchers. */
parse source . . script
script = strip(strip(script), 'B', '"')
root = left(script, lastpos('\', script))
if root = '' then do
  root = directory()
  if right(root, 1) \= '\' then root = root || '\'
end
bin = root || 'BIN'
if verify(bin, '^;,', 'M') > 0 then do
  say 'Extract the suite to a directory without a caret, semicolon, or comma.'
  exit 1
end

names = 'WORDCRFT DECKCRFT GRIDCRFT'
files = 'WORDCRFT.EXE DECKCRFT.EXE GRIDCRFT.EXE CRAFTPM.DLL'
do i = 1 to words(files)
  path = bin || '\' || word(files, i)
  if stream(path, 'C', 'QUERY EXISTS') = '' then do
    say 'Missing' path
    exit 1
  end
end

call RxFuncAdd 'SysLoadFuncs', 'REXXUTIL', 'SysLoadFuncs'
call SysLoadFuncs
folder = '<CRAFT_SUITE_OS2>'
if SysCreateObject('WPFolder', 'Craft Suite', '<WP_DESKTOP>', ,
                   'OBJECTID=' || folder || ';', 'U') = 0 then do
  say 'Could not create the Craft Suite folder on the Desktop.'
  exit 1
end

titles = 'WordCraft DeckCraft GridCraft'
failed = 0
do i = 1 to words(names)
  name = word(names, i)
  title = word(titles, i)
  base = bin || '\' || name
  setup = 'OBJECTID=<CRAFT_OS2_' || name || '>;' || ,
          'PROGTYPE=PM;EXENAME=' || base || '.EXE;STARTUPDIR=' || bin || ';'
  if stream(base || '.ICO', 'C', 'QUERY EXISTS') \= '' then do
    setup = setup || 'ICONFILE=' || base || '.ICO;'
  end
  if SysCreateObject('WPProgram', title, folder, setup, 'U') = 0 then do
    say 'Could not create the' title 'launcher.'
    failed = 1
  end
end
if failed = 0 then say 'Craft Suite launchers are ready in the Desktop folder.'
exit failed
