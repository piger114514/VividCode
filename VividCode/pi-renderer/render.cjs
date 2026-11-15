const MarkdownIt = require('markdown-it');
const {mathjax} = require('mathjax-full/js/mathjax.js');
const {TeX} = require('mathjax-full/js/input/tex.js');
const {SVG} = require('mathjax-full/js/output/svg.js');
const {liteAdaptor} = require('mathjax-full/js/adaptors/liteAdaptor.js');
const {RegisterHTMLHandler} = require('mathjax-full/js/handlers/html.js');
const {AllPackages} = require('mathjax-full/js/input/tex/AllPackages.js');
const adaptor=liteAdaptor(); RegisterHTMLHandler(adaptor);
const doc=mathjax.document('',{InputJax:new TeX({packages:AllPackages.filter(p=>!['require','autoload','html'].includes(p)),maxBuffer:10000,maxMacros:1000}),OutputJax:new SVG({fontCache:'none'})});
let input=''; process.stdin.setEncoding('utf8'); process.stdin.on('data',s=>{input+=s;if(input.length>2097152)process.exit(2)});
process.stdin.on('end',()=>{
 const images={}; const md=new MarkdownIt({html:false,linkify:false});
 const math=(tex,display)=>{try {const node=doc.convert(tex,{display}); const svg=adaptor.outerHTML(adaptor.firstChild(node)).replace(/(width|height)="([\d.]+)ex"/g,(_,key,n)=>key+'="'+Math.max(1,Math.ceil(Number(n)*8))+'px"'); const key='math:'+Object.keys(images).length; images[key]=svg.replace(/currentColor/g,'#eeeeee');return '<img src="'+key+'" />';}catch{return md.utils.escapeHtml(tex)}};
 md.inline.ruler.before('escape','math',(state,silent)=>{
  const at=state.pos, src=state.src; let close, begin;
  if(src.startsWith('\\(',at)){begin=at+2;close='\\)'}
  else if(src[at]==='$'&&src[at+1]!=='$'&&!/\s/.test(src[at+1]||' ')){begin=at+1;close='$'}else return false;
  let end=src.indexOf(close,begin);while(end>=0&&src[end-1]==='\\')end=src.indexOf(close,end+close.length);
  if(end<0||src.slice(begin,end).includes('\n'))return false;
  if(!silent){const token=state.push('math_inline','',0);token.content=src.slice(begin,end)}state.pos=end+close.length;return true;
 });
 md.renderer.rules.math_inline=(tokens,i)=>math(tokens[i].content,false);
 md.block.ruler.before('fence','math_block',(state,start,end,silent)=>{
  const first=state.src.slice(state.bMarks[start]+state.tShift[start],state.eMarks[start]);
  const open=first.startsWith('$$')?'$$':first.startsWith('\\[')?'\\[':null;if(!open)return false;
  const close=open==='$$'?'$$':'\\]';let content=first.slice(open.length),line=start;
  while(!content.includes(close)&&++line<end)content+='\n'+state.src.slice(state.bMarks[line],state.eMarks[line]);
  const stop=content.indexOf(close);if(stop<0)return false;if(!silent){const token=state.push('math_block','',0);token.content=content.slice(0,stop);token.block=true}state.line=line+1;return true;
 });
 md.renderer.rules.math_block=(tokens,i)=>'<p>'+math(tokens[i].content,true)+'</p>';
 md.renderer.rules.image=()=>''; // No network or local file image loads from model text.
 let payload;try{payload=JSON.parse(input)}catch{}
 const html=payload && Array.isArray(payload.messages)
  ? '<table width="100%" cellspacing="0" cellpadding="12" border="0">'+payload.messages.map((message,i)=>{
      const name=message.role==='user'?'你':'pi'; const color=message.role==='user'?'#2d2d2d':'#181818';
      return '<tr bgcolor="'+color+'"><td width="52" valign="top"><span style="font-size:14pt;font-weight:700;color:#ffffff">'+name+'</span></td><td valign="top">'+md.render(String(message.text||''))+'</td></tr>';
    }).join('')+'</table>'
  : md.render(input);
 process.stdout.write(JSON.stringify({html,images}));
});
