# Token-aware, fail-closed CarPlay envs editor. POSIX awk; no Python on QNX.
# Accepts JSON plus firmware '#' line comments. Validates the complete document
# before emitting anything; braces/quotes inside strings never alter scope.
# query=/path, extract=1, validate=1, or rewrite hook/exclude/exclude2/prefix.
# allow_absent=1 permits removal-only when envs contains no LD_PRELOAD entry.
# insert_if_absent=1 permits insertion of one LD_PRELOAD entry into the unique
# carplay.envs/env array after the complete document has validated.
function die(message) { bad=1; print "PRELOAD_CONFIG_ERROR: " message > "/dev/stderr"; exit 9 }
function hex(c) { return index("0123456789abcdef",tolower(c))-1 }
function lex(    c,q,v,e,h,k,n,start,opaque) {
  while (pos<=length(doc)) {
    c=substr(doc,pos,1)
    if(c ~ /[ \t\r\n]/) {pos++;continue}
    if(c=="#") {while(pos<=length(doc)&&substr(doc,pos,1)!="\n")pos++;continue}
    break
  }
  lo=pos; text=""; unicode_opaque=0
  if(pos>length(doc)){kind="EOF";hi=pos;return}
  c=substr(doc,pos,1)
  if(index("{}[],:",c)){kind=c;hi=pos++;return}
  if(c=="\"") {
    pos++;v=""
    while(pos<=length(doc)) {
      c=substr(doc,pos++,1)
      if(c=="\""){kind="S";text=v;hi=pos-1;return}
      if(c ~ /[[:cntrl:]]/)die("control character in string")
      if(c=="\\") {
        e=substr(doc,pos++,1)
        if(e=="\""||e=="\\"||e=="/")c=e
        else if(e=="b")c=sprintf("%c",8)
        else if(e=="f")c=sprintf("%c",12)
        else if(e=="n")c="\n"
        else if(e=="r")c="\r"
        else if(e=="t")c="\t"
        else if(e=="u") {
          h=substr(doc,pos,4);if(length(h)!=4||h ~ /[^0-9a-fA-F]/)die("invalid unicode escape")
          n=0;for(k=1;k<=4;k++)n=n*16+hex(substr(h,k,1));pos+=4
          if(n<128)c=sprintf("%c",n)
          else {c="\\u" h;unicode_opaque=1}
        } else die("invalid string escape")
      }
      v=v c
    }
    die("unterminated string")
  }
  q=substr(doc,pos)
  if(match(q,/^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?/)) {kind="N";hi=pos+RLENGTH-1;pos+=RLENGTH;return}
  if(match(q,/^(true|false|null)/)){kind="L";hi=pos+RLENGTH-1;pos+=RLENGTH;return}
  die("unexpected token at byte " pos)
}
function expect(w){if(kind!=w)die("expected " w " at byte " lo)}
function value(depth,scope,    id,key,keyopaque) {
  if(depth>64)die("nesting limit")
  if(kind=="{") {
    id=++object_id;lex()
    if(kind=="}"){lex();return}
    while(1) {
      expect("S");key=text;keyopaque=unicode_opaque
      if(seen[id SUBSEP key]++)die("duplicate object key")
      lex();expect(":");lex()
      if(key=="carplay"&&!keyopaque) {
        cp_count++;expect("{");value(depth+1,1)
      } else if((key=="envs"||key=="env")&&scope==1) {
        env_count++;expect("[");env_array(depth+1)
      } else value(depth+1,0)
      if(kind=="}"){lex();return}
      expect(",");lex()
    }
  }
  if(kind=="[") {
    lex();if(kind=="]"){lex();return}
    while(1){value(depth+1,0);if(kind=="]"){lex();return};expect(",");lex()}
  }
  if(kind=="S"||kind=="N"||kind=="L"){lex();return}
  die("missing value at byte " lo)
}
function env_array(depth,    items) {
  if(depth>64)die("nesting limit")
  env_open_at=lo;items=0
  lex()
  if(kind=="]"){env_close_at=lo;env_item_count=0;lex();return}
  while(1) {
    expect("S");items++
    if(index(text,"LD_PRELOAD=")==1) {
      if(unicode_opaque)die("non-ASCII unicode escapes in preload require explicit support")
      preload_count++;begin_at=lo;end_at=hi;preload=substr(text,12)
    }
    if(index(text,"LD_LIBRARY_PATH=")==1) {
      if(unicode_opaque)die("non-ASCII unicode escapes in library path require explicit support")
      libpath_count++;lib_begin_at=lo;lib_end_at=hi;libpath=substr(text,17)
    }
    lex()
    if(kind=="]"){env_close_at=lo;env_item_count=items;lex();return}
    expect(",");lex()
  }
}
function quoted(s,    out,c,i) {
  out="\""
  for(i=1;i<=length(s);i++) {
    c=substr(s,i,1)
    if(c=="\\"||c=="\"")out=out "\\" c
    else if(c=="\n")out=out "\\n"
    else if(c=="\r")out=out "\\r"
    else if(c=="\t")out=out "\\t"
    else if(c ~ /[[:cntrl:]]/)die("control character in preload")
    else out=out c
  }
  return out "\""
}
{doc=doc $0 "\n";if(length(doc)>1048576)die("configuration size limit")}
END {
  if(bad)exit 9
  pos=1;lex();expect("{");value(0,0);expect("EOF")
  if(cp_count!=1||env_count!=1||preload_count>1)die("expected one unambiguous carplay.envs scope")
  if(validate==1)exit 0
  count=split(preload,entries,":");found=0
  for(i=1;i<=count;i++)if(entries[i]==query&&query!="")found=1
  if(query!="")exit(found?0:1)
  if(extract!=""){if(preload_count==1){print preload;exit 0};exit 1}
  if(libdir!="") {
    if(libpath_count!=1)die("expected one unambiguous LD_LIBRARY_PATH entry")
    count=split(libpath,entries,":");out=libdir
    for(i=1;i<=count;i++){item=entries[i];if(item==""||item==libdir||item==legacy_libdir)continue;out=out ":" item}
    printf "%s%s%s",substr(doc,1,lib_begin_at-1),quoted("LD_LIBRARY_PATH=" out),substr(doc,lib_end_at+1)
    exit 0
  }
  if(!preload_count) {
    if(hook!=""&&insert_if_absent==1) {
      if(!env_close_at)die("carplay env array insertion point missing")
      addition=(env_item_count?",":"") quoted("LD_PRELOAD=" hook)
      printf "%s%s%s",substr(doc,1,env_close_at-1),addition,substr(doc,env_close_at)
      exit 0
    }
    if(allow_absent==1&&hook==""){printf "%s",doc;exit 0}
    die("preload absent in insertion mode")
  }
  out=hook
  for(i=1;i<=count;i++) {
    item=entries[i]
    if(item==""||item==hook||item==exclude||item==exclude2||(exclude_prefix!=""&&index(item,exclude_prefix)==1))continue
    out=out (out!=""?":":"") item
  }
  printf "%s%s%s",substr(doc,1,begin_at-1),quoted("LD_PRELOAD=" out),substr(doc,end_at+1)
}
