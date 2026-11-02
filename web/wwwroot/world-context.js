/* URL transitions preserve peer scopes and projection-specific extensions. */
(() => {
  'use strict';
  const change=(params,patch={})=>{
    const next=new URLSearchParams(params);
    for(const [key,value] of Object.entries(patch)){
      next.delete(key);
      if(Array.isArray(value))for(const item of value)next.append(key,String(item));
      else if(value!==null&&value!==undefined&&value!=='')next.set(key,String(value));
    }
    return next;
  };
  const address=(page,params,patch={})=>{const next=change(params,patch);return '#/'+page+(next.size?'?'+next:'')};
  window.LaplaceWorld=Object.freeze({change,address});
})();
