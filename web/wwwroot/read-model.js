/* Presence and match provenance are Native read facts, independent of labels. */
(() => {
  'use strict';
  const known = value => value?.recorded === true || ['tier0-cache', 'highway-cache'].includes(value?.storage);
  const matches = read => {
    if (read.discovery) {
      if (read.discovery.schema !== 'laplace-discovery/v1' || !Array.isArray(read.discovery.matches)) throw new Error('Unsupported Native discovery contract.');
      return read.discovery.matches.map(match => {
        if (!/^[a-f0-9]{32}$/i.test(match.id) || !known(match) || !['exact', 'member'].includes(match.match)) throw new Error('Native discovery returned a match without a known-content basis.');
        if (match.match === 'member' && (match.via !== read.id || !Number.isSafeInteger(match.ordinal) || match.ordinal < 0)) throw new Error('Native discovery returned an invalid member occurrence.');
        return match;
      });
    }
    // Older readers explicitly report root storage but not member presence.
    return known(read) ? [{ id: read.id, tier: read.tier, match: 'content', recorded: read.recorded, storage: read.storage }] : [];
  };
  window.LaplaceRead = Object.freeze({ known, matches });
})();
