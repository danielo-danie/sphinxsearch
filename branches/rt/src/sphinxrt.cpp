//
// $Id$
//

#include "sphinx.h"
#include "sphinxint.h"
#include "sphinxrt.h"
#include "sphinxsearch.h"

//////////////////////////////////////////////////////////////////////////

#define	COMPRESSED_WORDLIST		0
#define	COMPRESSED_DOCLIST		1
#define COMPRESSED_HITLIST		1

#if USE_64BIT
#define WORDID_MAX				U64C(0xffffffffffffffff)
#else
#define	WORDID_MAX				0xffffffffUL
#endif

/////////////////////////////////////////////////////////////////////////////

// !COMMIT replace with actual scoped locks impl later
#define RLOCK(_arg) ;
#define WLOCK(_arg) ;

// !COMMIT cleanup extern ref to sphinx.cpp
extern void sphSortDocinfos ( DWORD * pBuf, int iCount, int iStride );

// !COMMIT yes i am when debugging
#ifndef NDEBUG
#define PARANOID 1
#endif

//////////////////////////////////////////////////////////////////////////

static inline void ZipDword ( CSphVector<BYTE> & dOut, DWORD uValue )
{
	do 
	{
		BYTE bOut = BYTE( uValue & 0x7f );
		uValue >>= 7;
		if ( uValue )
			bOut |= 0x80;
		dOut.Add ( bOut );
	} while ( uValue );
}


static inline const BYTE * UnzipDword ( DWORD * pValue, const BYTE * pIn )
{
	DWORD uValue = 0;
	BYTE bIn;
	int iOff = 0;

	do 
	{
		bIn = *pIn++;
		uValue += ( bIn & 0x7f )<<iOff;
		iOff += 7;
	} while ( bIn & 0x80 );

	*pValue = uValue;
	return pIn;
}

//////////////////////////////////////////////////////////////////////////

struct CmpHit_fn
{
	inline int operator () ( const CSphWordHit & a, const CSphWordHit & b )
	{
		return 	( a.m_iWordID < b.m_iWordID ) ||
			( a.m_iWordID == b.m_iWordID && a.m_iDocID < b.m_iDocID ) || 
			( a.m_iWordID == b.m_iWordID && a.m_iDocID == b.m_iDocID && a.m_iWordPos < b.m_iWordPos );
	}
};


struct RtDoc_t
{
	SphDocID_t					m_uDocID;	///< my document id
	DWORD						m_uFields;	///< fields mask
	DWORD						m_uHits;	///< hit count
	DWORD						m_uHit;		///< either index into segment hits, or the only hit itself (if hit count is 1)
};


struct RtWord_t
{
	SphWordID_t					m_uWordID;	///< my keyword id
	DWORD						m_uDocs;	///< document count (for stats and/or BM25)
	DWORD						m_uHits;	///< hit count (for stats and/or BM25)
	DWORD						m_uDoc;		///< index into segment docs
};


struct RtSegment_t
{
protected:
	static const int			KLIST_ACCUM_THRESH	= 32;

public:
	static int					m_iSegments;	///< age tag sequence generator
	int							m_iTag;			///< segment age tag

#if COMPRESSED_WORDLIST
	CSphVector<BYTE>			m_dWords;
#else
	CSphVector<RtWord_t>		m_dWords;
#endif

#if COMPRESSED_DOCLIST
	CSphVector<BYTE>			m_dDocs;
#else
	CSphVector<RtDoc_t>			m_dDocs;
#endif

#if COMPRESSED_HITLIST
	CSphVector<BYTE>			m_dHits;
#else
	CSphVector<DWORD>			m_dHits;
#endif

	int							m_iRows;		///< number of actually allocated rows
	int							m_iAliveRows;	///< number of alive (non-killed) rows
	CSphVector<CSphRowitem>		m_dRows;		///< row data storage

	CSphVector<SphDocID_t>		m_dKlist;		///< sorted K-list
	CSphVector<SphDocID_t>		m_dKlistAccum;	///< unsorted K-list accumulator

	RtSegment_t ()
	{
		WLOCK ( m_iSegments );
		m_iTag = m_iSegments++;
		m_iRows = 0;
		m_iAliveRows = 0;
	}

	int GetSizeBytes () const
	{
		return
			m_dWords.GetLength()*sizeof(m_dWords[0]) +
			m_dDocs.GetLength()*sizeof(m_dDocs[0]) +
			m_dHits.GetLength()*sizeof(m_dHits[0]);
	}

	int GetMergeFactor () const
	{
		return m_iRows;
	}

	bool HasDocid ( SphDocID_t uDocid ) const;
	void DeleteDocument ( SphDocID_t uDocid );
	void OptimizeKlist ();
};

int RtSegment_t::m_iSegments = 0;


bool RtSegment_t::HasDocid ( SphDocID_t uDocid ) const
{
	// binary search through the rows
	int iStride = m_dRows.GetLength() / m_iRows;
	SphDocID_t uL = DOCINFO2ID(&m_dRows[0]);
	SphDocID_t uR = DOCINFO2ID(&m_dRows[m_dRows.GetLength()-iStride]);
	if ( uDocid==uL || uDocid==uR )
		return true;
	if ( uDocid<uL || uDocid>uR )
		return false;

	int iL = 0;
	int iR = m_iRows-1;
	while ( iR-iL>1 )
	{
		int iM = iL + (iR-iL)/2;
		SphDocID_t uM = DOCINFO2ID(&m_dRows[iM*iStride]);

		if ( uDocid==uM )
			return true;
		else if ( uDocid>uM )
			iL = iM;
		else
			iR = iM;
	}
	return false;
}


void RtSegment_t::DeleteDocument ( SphDocID_t uDocid )
{
	// do we have it at all?
	if ( !HasDocid ( uDocid ) )
		return;

	// do we kill it already?
	if ( m_dKlist.BinarySearch ( uDocid ) )
		return;

	ARRAY_FOREACH ( i, m_dKlistAccum )
		if ( m_dKlistAccum[i]==uDocid )
			return;

	// update the segment
	WLOCK ( this );
	m_dKlistAccum.Add ( uDocid );

	// merge and sort
	// OPTIMIZE? would sorting accum and merging be faster?
	if ( m_dKlistAccum.GetLength()==KLIST_ACCUM_THRESH )
		OptimizeKlist(); // FIXME? nested wlock

	m_iAliveRows--;
}


void RtSegment_t::OptimizeKlist ()
{
	WLOCK ( this );
	ARRAY_FOREACH ( i, m_dKlistAccum )
		m_dKlist.Add ( m_dKlistAccum[i] );
	m_dKlist.Sort ();
	m_dKlistAccum.Resize ( 0 );
}

//////////////////////////////////////////////////////////////////////////

#if COMPRESSED_DOCLIST

struct RtDocWriter_t
{
	CSphVector<BYTE> *	m_pDocs;
	SphDocID_t			m_uLastDocID;

	explicit RtDocWriter_t ( RtSegment_t * pSeg )
		: m_pDocs ( &pSeg->m_dDocs )
		, m_uLastDocID ( 0 )
	{}

	void ZipDoc ( const RtDoc_t & tDoc )
	{
		CSphVector<BYTE> & dDocs = *m_pDocs;
		ZipDword ( dDocs, tDoc.m_uDocID - m_uLastDocID ); // !COMMIT might be qword
		m_uLastDocID = tDoc.m_uDocID;
		ZipDword ( dDocs, tDoc.m_uFields );
		ZipDword ( dDocs, tDoc.m_uHits );
		if ( tDoc.m_uHits==1 )
		{
			ZipDword ( dDocs, tDoc.m_uHit & 0xffffffUL );
			ZipDword ( dDocs, tDoc.m_uHit>>24 );
		} else
			ZipDword ( dDocs, tDoc.m_uHit );
	}

	DWORD ZipDocPtr () const
	{
		return m_pDocs->GetLength();
	}

	void ZipRestart ()
	{
		m_uLastDocID = 0;
	}
};

struct RtDocReader_t
{
	const BYTE *	m_pDocs;
	int				m_iLeft;
	RtDoc_t			m_tDoc;

	explicit RtDocReader_t ( const RtSegment_t * pSeg, const RtWord_t & tWord )
	{
		m_pDocs = &pSeg->m_dDocs[0] + tWord.m_uDoc;
		m_iLeft = tWord.m_uDocs;
		m_tDoc.m_uDocID = 0;
	}

	const RtDoc_t * UnzipDoc ()
	{
		if ( !m_iLeft )
			return NULL;

		const BYTE * pIn = m_pDocs;
		SphDocID_t uDeltaID;			 
		pIn = UnzipDword ( &uDeltaID, pIn ); // !COMMIT might be qword
		m_tDoc.m_uDocID += uDeltaID;
		pIn = UnzipDword ( &m_tDoc.m_uFields, pIn );
		pIn = UnzipDword ( &m_tDoc.m_uHits, pIn );
		if ( m_tDoc.m_uHits==1 )
		{
			DWORD a, b;
			pIn = UnzipDword ( &a, pIn );
			pIn = UnzipDword ( &b, pIn );
			m_tDoc.m_uHit = a + ( b<<24 );
		} else
			pIn = UnzipDword ( &m_tDoc.m_uHit, pIn );
		m_pDocs = pIn;

		m_iLeft--;
		return &m_tDoc;
	}
};

#else

struct RtDocWriter_t
{
	CSphVector<RtDoc_t> *	m_pDocs;

	explicit				RtDocWriter_t ( RtSegment_t * pSeg )	: m_pDocs ( &pSeg->m_dDocs ) {}
	void					ZipDoc ( const RtDoc_t & tDoc )			{ m_pDocs->Add ( tDoc ); }
	DWORD					ZipDocPtr () const						{ return m_pDocs->GetLength(); }
	void					ZipRestart ()							{}
};


struct RtDocReader_t
{
	const RtDoc_t *	m_pDocs;
	int				m_iPos;
	int				m_iMax;

	explicit RtDocReader_t ( const RtSegment_t * pSeg, const RtWord_t & tWord )
	{
		m_pDocs = &pSeg->m_dDocs[0];
		m_iPos = tWord.m_uDoc;
		m_iMax = tWord.m_uDoc + tWord.m_uDocs;
	}

	const RtDoc_t * UnzipDoc ()
	{
		return m_iPos<m_iMax ? m_pDocs + m_iPos++ : NULL;
	}
};

#endif // COMPRESSED_DOCLIST


#if COMPRESSED_WORDLIST

struct RtWordWriter_t
{
	CSphVector<BYTE> *	m_pWords;
	SphWordID_t			m_uLastWordID;
	DWORD				m_uLastDoc;

	explicit RtWordWriter_t ( RtSegment_t * pSeg )
		: m_pWords ( &pSeg->m_dWords )
		, m_uLastWordID ( 0 )
		, m_uLastDoc ( 0 )
	{}

	void ZipWord ( const RtWord_t & tWord )
	{
		CSphVector<BYTE> & tWords = *m_pWords;
		ZipDword ( tWords, tWord.m_uWordID - m_uLastWordID ); // !COMMIT might be qword
		ZipDword ( tWords, tWord.m_uDocs );
		ZipDword ( tWords, tWord.m_uHits );
		ZipDword ( tWords, tWord.m_uDoc - m_uLastDoc );
		m_uLastWordID = tWord.m_uWordID;
		m_uLastDoc = tWord.m_uDoc;
	}
};


struct RtWordReader_t
{
	const BYTE *	m_pCur;
	const BYTE *	m_pMax;
	RtWord_t		m_tWord;	

	explicit RtWordReader_t ( const RtSegment_t * pSeg )
	{
		m_pCur = &pSeg->m_dWords[0];
		m_pMax = m_pCur + pSeg->m_dWords.GetLength();

		m_tWord.m_uWordID = 0;
		m_tWord.m_uDoc = 0;
	}

	const RtWord_t * UnzipWord ()
	{
		if ( m_pCur>=m_pMax )
			return NULL;

		const BYTE * pIn = m_pCur;
		DWORD uDeltaID, uDeltaDoc;
		pIn = UnzipDword ( &uDeltaID, pIn ); // !COMMIT might be qword
		pIn = UnzipDword ( &m_tWord.m_uDocs, pIn );
		pIn = UnzipDword ( &m_tWord.m_uHits, pIn );
		pIn = UnzipDword ( &uDeltaDoc, pIn );
		m_pCur = pIn;

		m_tWord.m_uWordID += uDeltaID;
		m_tWord.m_uDoc += uDeltaDoc;
		return &m_tWord;
	}
};

#else

struct RtWordWriter_t
{
	CSphVector<RtWord_t> *	m_pWords;

	explicit				RtWordWriter_t ( RtSegment_t * pSeg )	: m_pWords ( &pSeg->m_dWords ) {}
	void					ZipWord ( const RtWord_t & tWord )		{ m_pWords->Add ( tWord ); }
};


struct RtWordReader_t
{
	const RtWord_t *	m_pCur;
	const RtWord_t *	m_pMax;

	explicit RtWordReader_t ( const RtSegment_t * pSeg )
	{
		m_pCur = &pSeg->m_dWords[0];
		m_pMax = m_pCur + pSeg->m_dWords.GetLength();
	}

	const RtWord_t * UnzipWord ()
	{
		return m_pCur<m_pMax ? m_pCur++ : NULL;
	}
};

#endif // COMPRESSED_WORDLIST


#if COMPRESSED_HITLIST

struct RtHitWriter_t
{
	CSphVector<BYTE> *	m_pHits;
	DWORD				m_uLastHit;

	explicit RtHitWriter_t ( RtSegment_t * pSeg )
		: m_pHits ( &pSeg->m_dHits )
		, m_uLastHit ( 0 )
	{}

	void ZipHit ( DWORD uValue )
	{
		ZipDword ( *m_pHits, uValue - m_uLastHit );
		m_uLastHit = uValue;
	}

	void ZipRestart ()
	{
		m_uLastHit = 0;
	}

	DWORD ZipHitPtr () const
	{
		return m_pHits->GetLength();
	}
};


struct RtHitReader_t
{
	const BYTE *	m_pCur;
	DWORD			m_iLeft;
	DWORD			m_uLast;

	RtHitReader_t ()
		: m_pCur ( NULL )
		, m_iLeft ( 0 )
		, m_uLast ( 0 )
	{}

	explicit RtHitReader_t ( const RtSegment_t * pSeg, const RtDoc_t * pDoc )
	{
		m_pCur = &pSeg->m_dHits [ pDoc->m_uHit ];
		m_iLeft = pDoc->m_uHits;
		m_uLast = 0;
	}

	DWORD UnzipHit ()
	{
		if ( !m_iLeft )
			return 0;

		DWORD uValue;
		m_pCur = UnzipDword ( &uValue, m_pCur );
		m_uLast += uValue;
		m_iLeft--;
		return m_uLast;
	}
};


struct RtHitReader2_t : public RtHitReader_t
{
	const BYTE * m_pBase;

	RtHitReader2_t ()
		: m_pBase ( NULL )
	{}

	void Seek ( SphOffset_t uOff, int iHits )
	{
		m_pCur = m_pBase + uOff;
		m_iLeft = iHits;
		m_uLast = 0;
	}
};

#else

struct RtHitWriter_t
{
	CSphVector<DWORD> *	m_pHits;

	explicit			RtHitWriter_t ( RtSegment_t * pSeg )	: m_pHits ( &pSeg->m_dHits ) {}
	void				ZipHit ( DWORD uValue )					{ m_pHits->Add ( uValue ); }
	void				ZipRestart ()							{}
	DWORD				ZipHitPtr () const						{ return m_pHits->GetLength(); }
};

struct RtHitReader_t
{
	const DWORD * m_pCur;
	const DWORD * m_pMax;

	explicit RtHitReader_t ( const RtSegment_t * pSeg, const RtDoc_t * pDoc )
	{
		m_pCur = &pSeg->m_dHits [ pDoc->m_uHit ];
		m_pMax = m_pCur + pDoc->m_uHits;
	}

	DWORD UnzipHit ()
	{
		return m_pCur<m_pMax ? *m_pCur++ : 0;
	}
};

#endif // COMPRESSED_HITLIST

//////////////////////////////////////////////////////////////////////////

struct RtIndex_t : public ISphRtIndex, public ISphNoncopyable
{
private:
	CSphVector<CSphWordHit>		m_dAccum;
	CSphVector<CSphRowitem>		m_dAccumRows;
	int							m_iAccumDocs;

	CSphVector<RtSegment_t*>	m_pSegments;

	const int					m_iStride;

public:
	explicit					RtIndex_t ( const CSphSchema & tSchema );
	virtual						~RtIndex_t ();

	void						AddDocument ( const CSphVector<CSphString> & dFields, const CSphDocInfo & tDoc );
	void						AddDocument ( const CSphVector<CSphWordHit> & dHits, const CSphDocInfo & tDoc );
	void						DeleteDocument ( SphDocID_t uDoc );
	void						Commit ();

private:
	RtSegment_t *				CreateSegment ();
	RtSegment_t *				MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2 );
	const RtWord_t *			CopyWord ( RtSegment_t * pDst, RtWordWriter_t & tOutWord, const RtSegment_t * pSrc, const RtWord_t * pWord, RtWordReader_t & tInWord );
	void						MergeWord ( RtSegment_t * pDst, const RtSegment_t * pSrc1, const RtWord_t * pWord1, const RtSegment_t * pSrc2, const RtWord_t * pWord2, RtWordWriter_t & tOut );
	void						CopyDoc ( RtSegment_t * pSeg, RtDocWriter_t & tOutDoc, RtWord_t * pWord, const RtSegment_t * pSrc, const RtDoc_t * pDoc );

	void						DumpToDisk ( const char * sFilename );

public:
#pragma warning(push,1)
#pragma warning(disable:4100)
	virtual SphAttr_t *			GetKillList () const			{ return NULL; }
	virtual int					GetKillListSize () const		{ return 0; }

	virtual int					Build ( const CSphVector<CSphSource*> & dSources, int iMemoryLimit, int iWriteBuffer ) { return 0; }
	virtual bool				Merge ( CSphIndex * pSource, CSphVector<CSphFilterSettings> & dFilters, bool bMergeKillLists ) { return false; }

	virtual const CSphSchema *	Prealloc ( bool bMlock, CSphString & sWarning ) { return &m_tSchema; }
	virtual void				Dealloc () {}
	virtual bool				Preread () { return true; }
	virtual void				SetBase ( const char * sNewBase ) {}
	virtual bool				Rename ( const char * sNewBase ) { return true; }
	virtual bool				Lock () { return true; }
	virtual void				Unlock () {}
	virtual bool				Mlock () { return true; }

	virtual int					UpdateAttributes ( const CSphAttrUpdate & tUpd ) { return -1; }
	virtual bool				SaveAttributes () { return false; }

	virtual void				DebugDumpHeader ( FILE * fp, const char * sHeaderName ) {}
	virtual void				DebugDumpDocids ( FILE * fp ) {}
	virtual void				DebugDumpHitlist ( FILE * fp, const char * sKeyword ) {}
#pragma warning(pop)

public:
	virtual ISphQword *					QwordSpawn () const;
	virtual bool						QwordSetup ( ISphQword * pQword, const ISphQwordSetup * pSetup ) const;
	virtual bool						EarlyReject ( CSphMatch & ) const;
	virtual const CSphSourceStats &		GetStats () const { return m_tStats; }

	virtual bool				MultiQuery ( CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters, ISphMatchSorter ** ppSorters );
	virtual bool				GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, bool bGetStats );

	virtual CSphQueryResult *	Query ( CSphQuery * pQuery );
	virtual bool				QueryEx ( CSphQuery * pQuery, CSphQueryResult * pResult, ISphMatchSorter * pTop );
	void						BindWeights ( const CSphQuery * pQuery );

	void						CopyDocinfo ( CSphMatch & tMatch, const DWORD * pFound ) const;
	const CSphRowitem *			FindDocinfo ( const RtSegment_t * pSeg, SphDocID_t uDocID ) const;

protected:
	CSphSourceStats				m_tStats;

	// searching-only, per-query
	int							m_iWeights;						///< search query field weights count
	int							m_dWeights [ SPH_MAX_FIELDS ];	///< search query field weights
};


RtIndex_t::RtIndex_t ( const CSphSchema & tSchema )
	: ISphRtIndex ( "rtindex" )
	, m_iAccumDocs ( 0 )
	, m_iStride ( DOCINFO_IDSIZE + tSchema.GetRowSize() )
{
	m_tSchema = tSchema;
	m_dAccum.Reserve ( 2*1024*1024 );
}


RtIndex_t::~RtIndex_t ()
{
	ARRAY_FOREACH ( i, m_pSegments )
		SafeDelete ( m_pSegments[i] );
}


class CSphSource_StringVector : public CSphSource_Document
{
public:
	explicit			CSphSource_StringVector ( const CSphVector<CSphString> & dFields, const CSphSchema & tSchema );
	virtual				~CSphSource_StringVector () {}

	virtual bool		Connect ( CSphString & ) { return true; }
	virtual void		Disconnect () {}

	virtual bool		HasAttrsConfigured () { return false; }
	virtual bool		IterateHitsStart ( CSphString & ) { return true; }

	virtual bool		IterateMultivaluedStart ( int, CSphString & ) { return false; }
	virtual bool		IterateMultivaluedNext () { return false; }

	virtual bool		IterateFieldMVAStart ( int, CSphString & ) { return false; }
	virtual bool		IterateFieldMVANext () { return false; }

	virtual bool		IterateKillListStart ( CSphString & ) { return false; }
	virtual bool		IterateKillListNext ( SphDocID_t & ) { return false; }

	virtual BYTE **		NextDocument ( CSphString & ) { return &m_dFields[0]; }

protected:
	CSphVector<BYTE *>	m_dFields;
};

CSphSource_StringVector::CSphSource_StringVector ( const CSphVector<CSphString> & dFields, const CSphSchema & tSchema )
	: CSphSource_Document ( "$stringvector" )
{
	m_tSchema = tSchema;

	m_dFields.Resize ( 1+dFields.GetLength() );
	ARRAY_FOREACH ( i, dFields )
		m_dFields[i] = (BYTE*) dFields[i].cstr();
	m_dFields [ dFields.GetLength() ] = NULL;
}

void RtIndex_t::AddDocument ( const CSphVector<CSphString> & dFields, const CSphDocInfo & tDoc )
{
	if ( !tDoc.m_iDocID )
		return;

	CSphSource_StringVector tSrc ( dFields, m_tSchema );
	tSrc.SetTokenizer ( m_pTokenizer );
	tSrc.SetDict ( m_pDict );

	tSrc.m_tDocInfo = tDoc;
	if ( !tSrc.IterateHitsNext ( m_sLastError ) )
		return;

	AddDocument ( tSrc.m_dHits, tDoc );
}


void RtIndex_t::AddDocument ( const CSphVector<CSphWordHit> & dHits, const CSphDocInfo & tDoc )
{
	// kill existing copies
	DeleteDocument ( tDoc.m_iDocID );

	// !COMMIT make Add/Commit local to given thread
	if ( !dHits.GetLength() )
		return;

	// accumulate row data
	assert ( DOCINFO_IDSIZE + tDoc.m_iRowitems == m_iStride );
	m_dAccumRows.Resize ( m_dAccumRows.GetLength() + m_iStride );

	CSphRowitem * pRow = &m_dAccumRows [ m_dAccumRows.GetLength() - m_iStride ];
	DOCINFOSETID ( pRow, tDoc.m_iDocID );

	int iToCopy = Min ( m_iStride-DOCINFO_IDSIZE, tDoc.m_iRowitems );
	CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);

	for ( int i=0; i<iToCopy; i++ )
		pAttrs[i] = tDoc.m_pRowitems[i];

	for ( int i=iToCopy; i<m_iStride-DOCINFO_IDSIZE; i++)
		pAttrs[i] = 0;

	// accumulate hits
	ARRAY_FOREACH ( i, dHits )
		m_dAccum.Add ( dHits[i] );

	m_iAccumDocs++;
}


RtSegment_t * RtIndex_t::CreateSegment ()
{
	RtSegment_t * pSeg = new RtSegment_t ();

	CSphWordHit tClosingHit;
	tClosingHit.m_iWordID = WORDID_MAX;
	tClosingHit.m_iDocID = DOCID_MAX;
	tClosingHit.m_iWordPos = 1;
	m_dAccum.Add ( tClosingHit );
	m_dAccum.Sort ( CmpHit_fn() );

	RtDoc_t tDoc;
	tDoc.m_uDocID = 0;
	tDoc.m_uFields = 0;
	tDoc.m_uHits = 0;
	tDoc.m_uHit = 0;

	RtWord_t tWord;
	tWord.m_uWordID = 0;
	tWord.m_uDocs = 0;
	tWord.m_uHits = 0;
	tWord.m_uDoc = 0;

	RtDocWriter_t tOutDoc ( pSeg );
	RtWordWriter_t tOutWord ( pSeg );
	RtHitWriter_t tOutHit ( pSeg );

	DWORD uEmbeddedHit = 0;
	ARRAY_FOREACH ( i, m_dAccum )
	{
		const CSphWordHit & tHit = m_dAccum[i];

		// new keyword or doc; flush current doc
		if ( tHit.m_iWordID!=tWord.m_uWordID || tHit.m_iDocID!=tDoc.m_uDocID )
		{
			if ( tDoc.m_uDocID )
			{
				tWord.m_uDocs++;
				tWord.m_uHits += tDoc.m_uHits;

				if ( uEmbeddedHit )
				{
					assert ( tDoc.m_uHits==1 );
					tDoc.m_uHit = uEmbeddedHit;
				}

				tOutDoc.ZipDoc ( tDoc );
				tDoc.m_uFields = 0;
				tDoc.m_uHits = 0;
				tDoc.m_uHit = tOutHit.ZipHitPtr();
			}

			tDoc.m_uDocID = tHit.m_iDocID;
			tOutHit.ZipRestart ();
			uEmbeddedHit = 0;
		}

		// new keyword; flush current keyword
		if ( tHit.m_iWordID!=tWord.m_uWordID )
		{
			tOutDoc.ZipRestart ();
			if ( tWord.m_uWordID )
				tOutWord.ZipWord (  tWord );

			tWord.m_uWordID = tHit.m_iWordID;
			tWord.m_uDocs = 0;
			tWord.m_uHits = 0;
			tWord.m_uDoc = tOutDoc.ZipDocPtr();
		}

		// just a new hit
		if ( !tDoc.m_uHits )
		{
			uEmbeddedHit = tHit.m_iWordPos;
		} else
		{
			if ( uEmbeddedHit )
			{
				tOutHit.ZipHit ( uEmbeddedHit );
				uEmbeddedHit = 0;
			}

			tOutHit.ZipHit ( tHit.m_iWordPos );
		}

		tDoc.m_uFields |= 1UL << ( tHit.m_iWordPos>>24 ); // !COMMIT HIT2LCS()
		tDoc.m_uHits++;
	}

	pSeg->m_iRows = m_iAccumDocs;
	pSeg->m_iAliveRows = m_iAccumDocs;

	// copy and sort attributes
	pSeg->m_dRows.SwapData ( m_dAccumRows );
	sphSortDocinfos ( &pSeg->m_dRows[0], pSeg->m_dRows.GetLength()/m_iStride, m_iStride );

	// clean up accumulators
	m_dAccum.Resize ( 0 );
	m_dAccumRows.Resize ( 0 );
	m_iAccumDocs = 0;
	return pSeg;
}


const RtWord_t * RtIndex_t::CopyWord ( RtSegment_t * pDst, RtWordWriter_t & tOutWord, const RtSegment_t * pSrc, const RtWord_t * pWord, RtWordReader_t & tInWord )
{
	RtDocReader_t tInDoc ( pSrc, *pWord );
	RtDocWriter_t tOutDoc ( pDst );

	RtWord_t tNewWord = *pWord;
	tNewWord.m_uDoc = tOutDoc.ZipDocPtr();

	// copy docs
	for ( ;; )
	{
		const RtDoc_t * pDoc = tInDoc.UnzipDoc();
		if ( !pDoc )
			break;

		// apply klist
		assert ( pSrc->m_dKlistAccum.GetLength()==0 );
		if ( pSrc->m_dKlist.BinarySearch ( pDoc->m_uDocID ) )
		{
			tNewWord.m_uDocs--;
			tNewWord.m_uHits -= pDoc->m_uHits;
			continue;
		}

		// short route, single embedded hit
		if ( pDoc->m_uHits==1 )
		{
			tOutDoc.ZipDoc ( *pDoc );
			continue;
		}

		// long route, copy hits
		RtHitWriter_t tOutHit ( pDst );
		RtHitReader_t tInHit ( pSrc, pDoc );

		RtDoc_t tDoc = *pDoc;
		tDoc.m_uHit = tOutHit.ZipHitPtr();

		// OPTIMIZE? decode+memcpy?
		for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
			tOutHit.ZipHit ( uValue );

		// copy doc
		tOutDoc.ZipDoc ( tDoc );
	}

	// append word to the dictionary
	if ( tNewWord.m_uDocs )
		tOutWord.ZipWord ( tNewWord );

	// move forward
	return tInWord.UnzipWord ();
}


void RtIndex_t::CopyDoc ( RtSegment_t * pSeg, RtDocWriter_t & tOutDoc, RtWord_t * pWord, const RtSegment_t * pSrc, const RtDoc_t * pDoc )
{
	pWord->m_uDocs++;
	pWord->m_uHits += pDoc->m_uHits;

	if ( pDoc->m_uHits==1 )
	{
		tOutDoc.ZipDoc ( *pDoc );
		return;
	}

	RtHitWriter_t tOutHit ( pSeg );
	RtHitReader_t tInHit ( pSrc, pDoc );

	RtDoc_t tDoc = *pDoc;
	tDoc.m_uHit = tOutHit.ZipHitPtr();
	tOutDoc.ZipDoc ( tDoc );

	// OPTIMIZE? decode+memcpy?
	for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
		tOutHit.ZipHit ( uValue );
}


void RtIndex_t::MergeWord ( RtSegment_t * pSeg, const RtSegment_t * pSrc1, const RtWord_t * pWord1, const RtSegment_t * pSrc2, const RtWord_t * pWord2, RtWordWriter_t & tOut )
{
	assert ( pWord1->m_uWordID==pWord2->m_uWordID );

	RtDocWriter_t tOutDoc ( pSeg );

	RtWord_t tWord;
	tWord.m_uWordID = pWord1->m_uWordID;
	tWord.m_uDocs = 0;
	tWord.m_uHits = 0;
	tWord.m_uDoc = tOutDoc.ZipDocPtr();

	RtDocReader_t tIn1 ( pSrc1, *pWord1 );
	RtDocReader_t tIn2 ( pSrc2, *pWord2 );
	const RtDoc_t * pDoc1 = tIn1.UnzipDoc();
	const RtDoc_t * pDoc2 = tIn2.UnzipDoc();

	while ( pDoc1 || pDoc2 )
	{
		if ( pDoc1 && pDoc2 && pDoc1->m_uDocID==pDoc2->m_uDocID )
		{
			// dupe, must (!) be killed in the first segment, might be in both
			assert ( pSrc1->m_dKlist.BinarySearch ( pDoc1->m_uDocID ) );
			if ( !pSrc2->m_dKlist.BinarySearch ( pDoc2->m_uDocID ) )
				CopyDoc ( pSeg, tOutDoc, &tWord, pSrc2, pDoc2 );
			pDoc2 = tIn2.UnzipDoc();

		} else if ( pDoc1 && ( !pDoc2 || pDoc1->m_uDocID < pDoc2->m_uDocID ) )
		{
			// winner from the first segment
			if ( !pSrc1->m_dKlist.BinarySearch ( pDoc1->m_uDocID ) )
				CopyDoc ( pSeg, tOutDoc, &tWord, pSrc1, pDoc1 );
			pDoc1 = tIn1.UnzipDoc();

		} else
		{
			// winner from the second segment
			assert ( pDoc2 && ( !pDoc1 || pDoc2->m_uDocID < pDoc1->m_uDocID ) );
			if ( !pSrc2->m_dKlist.BinarySearch ( pDoc2->m_uDocID ) )
				CopyDoc ( pSeg, tOutDoc, &tWord, pSrc2, pDoc2 );
			pDoc2 = tIn2.UnzipDoc();
		}
	}

	if ( tWord.m_uDocs )
		tOut.ZipWord ( tWord );
}


#if PARANOID
static void CheckSegmentRows ( const RtSegment_t * pSeg, int iStride )
{
	const CSphVector<CSphRowitem> & dRows = pSeg->m_dRows; // shortcut
	for ( int i=iStride; i<dRows.GetLength(); i+=iStride )
		assert ( DOCINFO2ID(&dRows[i]) > DOCINFO2ID(&dRows[i-iStride]) );
}
#endif


struct RtRowIterator_t : public ISphNoncopyable
{
protected:
	const CSphRowitem * m_pRow;
	const CSphRowitem * m_pRowMax;
	const SphDocID_t * m_pKlist;
	const SphDocID_t * m_pKlistMax;
	const int m_iStride;

public:
	explicit RtRowIterator_t ( const RtSegment_t * pSeg, int iStride )
		: m_pRow ( &pSeg->m_dRows[0] )
		, m_pRowMax ( &pSeg->m_dRows[0] + pSeg->m_dRows.GetLength() )
		, m_pKlist ( pSeg->m_dKlist.GetLength()==0 ? NULL : &pSeg->m_dKlist[0] )
		, m_pKlistMax ( pSeg->m_dKlist.GetLength()==0 ? NULL : &pSeg->m_dKlist[0] + pSeg->m_dKlist.GetLength() )
		, m_iStride ( iStride )
	{}

	const CSphRowitem * GetNextAliveRow ()
	{
		if ( m_pRow>=m_pRowMax )
			return NULL;

		while ( m_pKlist<m_pKlistMax )
		{
			SphDocID_t uID = DOCINFO2ID(m_pRow);
			while ( m_pKlist<m_pKlistMax && *m_pKlist<uID )
				m_pKlist++;

			assert ( m_pKlist>=m_pKlistMax || *m_pKlist>=uID );
			if (!( m_pKlist<m_pKlistMax && *m_pKlist==uID ))
				break;

			m_pKlist++;
			m_pRow += m_iStride;
			if ( m_pRow>=m_pRowMax )
				return NULL;
		}

		m_pRow += m_iStride;
		return m_pRow-m_iStride;
	}
};


RtSegment_t * RtIndex_t::MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2 )
{
	RLOCK ( pSeg1 );
	RLOCK ( pSeg2 );

	// check that k-lists are optimized
	assert ( pSeg1->m_dKlistAccum.GetLength()==0 );
	assert ( pSeg2->m_dKlistAccum.GetLength()==0 );

	if ( pSeg1->m_iTag > pSeg2->m_iTag )
		Swap ( pSeg1, pSeg2 );

	RtSegment_t * pSeg = new RtSegment_t ();

	////////////////////
	// merge attributes
	////////////////////

	// check that all the IDs are in proper asc order
#if PARANOID
	CheckSegmentRows ( pSeg1, m_iStride );
	CheckSegmentRows ( pSeg2, m_iStride );
#endif

	// just a shortcut
	CSphVector<CSphRowitem> & dRows = pSeg->m_dRows;

	// we might need less because of dupes, but we can not know yet
	dRows.Reserve ( pSeg1->m_dRows.GetLength() + pSeg2->m_dRows.GetLength() );

	RtRowIterator_t tIt1 ( pSeg1, m_iStride );
	RtRowIterator_t tIt2 ( pSeg2, m_iStride );

	const CSphRowitem * pRow1 = tIt1.GetNextAliveRow();
	const CSphRowitem * pRow2 = tIt2.GetNextAliveRow();

	while ( pRow1 || pRow2 )
	{
		if ( !pRow2 || ( pRow1 && pRow2 && DOCINFO2ID(pRow1)<DOCINFO2ID(pRow2) ) )
		{
			assert ( pRow1 );
			for ( int i=0; i<m_iStride; i++ )
				dRows.Add ( *pRow1++ );
			pRow1 = tIt1.GetNextAliveRow();
		} else
		{
			assert ( pRow2 );
			assert ( !pRow1 || ( DOCINFO2ID(pRow1)!=DOCINFO2ID(pRow2) ) ); // all dupes must be killed and skipped by the iterator
			for ( int i=0; i<m_iStride; i++ )
				dRows.Add ( *pRow2++ );
			pRow2 = tIt2.GetNextAliveRow();
		}
		pSeg->m_iRows++;
		pSeg->m_iAliveRows++;
	}

	assert ( pSeg->m_iRows*m_iStride==pSeg->m_dRows.GetLength() );
#if PARANOID
	CheckSegmentRows ( pSeg, m_iStride );
#endif

	//////////////////
	// merge keywords
	//////////////////

	pSeg->m_dWords.Reserve ( pSeg1->m_dWords.GetLength() + pSeg2->m_dWords.GetLength() );
	pSeg->m_dDocs.Reserve ( pSeg1->m_dDocs.GetLength() + pSeg2->m_dDocs.GetLength() );
	pSeg->m_dHits.Reserve ( pSeg1->m_dHits.GetLength() + pSeg2->m_dHits.GetLength() );

	RtWordWriter_t tOut ( pSeg );
	RtWordReader_t tIn1 ( pSeg1 );
	RtWordReader_t tIn2 ( pSeg2 );
	const RtWord_t * pWords1 = tIn1.UnzipWord ();
	const RtWord_t * pWords2 = tIn2.UnzipWord ();

	// merge while there are common words
	for ( ;; )
	{
		while ( pWords1 && pWords2 && pWords1->m_uWordID!=pWords2->m_uWordID )
			if ( pWords1->m_uWordID < pWords2->m_uWordID )
				pWords1 = CopyWord ( pSeg, tOut, pSeg1, pWords1, tIn1 );
			else
				pWords2 = CopyWord ( pSeg, tOut, pSeg2, pWords2, tIn2 );

		if ( !pWords1 || !pWords2 )
			break;

		assert ( pWords1 && pWords2 && pWords1->m_uWordID==pWords2->m_uWordID );
		MergeWord ( pSeg, pSeg1, pWords1, pSeg2, pWords2, tOut );
		pWords1 = tIn1.UnzipWord();
		pWords2 = tIn2.UnzipWord();
	}

	// copy tails
	while ( pWords1 ) pWords1 = CopyWord ( pSeg, tOut, pSeg1, pWords1, tIn1 );
	while ( pWords2 ) pWords2 = CopyWord ( pSeg, tOut, pSeg2, pWords2, tIn2 );

	assert ( pSeg->m_dRows.GetLength() );
	assert ( pSeg->m_iRows );
	assert ( pSeg->m_iAliveRows );
	return pSeg;
}


struct CmpSegments_fn
{
	inline int operator () ( const RtSegment_t * a, const RtSegment_t * b )
	{
		return a->GetMergeFactor() > b->GetMergeFactor();
	}
};


void RtIndex_t::Commit ()
{
	// !COMMIT make Add/Commit local to given thread
	WLOCK ( this );

	if ( !m_dAccum.GetLength() )
		return;

	m_tStats.m_iTotalDocuments += m_iAccumDocs;

	RtSegment_t * pNewSeg = CreateSegment();
	assert ( pNewSeg->m_iRows );
	assert ( pNewSeg->m_iAliveRows );

	CSphVector<RtSegment_t*> dSegments;
	dSegments = m_pSegments;
	dSegments.Add ( pNewSeg );

	CSphVector<RtSegment_t*> dToKill;

	const int MAX_SEGMENTS = 8;
	for ( ;; )
	{
		dSegments.Sort ( CmpSegments_fn() );

		// unconditionally merge if there's too much segments now
		// conditionally merge if smallest segment has grown too large
		// otherwise, we're done
		int iLen = dSegments.GetLength();
		if (!( iLen>MAX_SEGMENTS || ( iLen>=2 && dSegments[iLen-1]->GetMergeFactor()*2 > dSegments[iLen-2]->GetMergeFactor() ) ))
			break;

		RtSegment_t * pA = dSegments.Pop();
		RtSegment_t * pB = dSegments.Pop();
		pA->OptimizeKlist();
		pB->OptimizeKlist();
		dSegments.Add ( MergeSegments ( pA, pB ) );
		dToKill.Add ( pA );
		dToKill.Add ( pB );
	}

	Swap ( m_pSegments, dSegments ); // !COMMIT atomic
	ARRAY_FOREACH ( i, dToKill )
		SafeDelete ( dToKill[i] ); // unused now
}


void RtIndex_t::DeleteDocument ( SphDocID_t uDoc )
{
	RLOCK ( this );
	ARRAY_FOREACH ( i, m_pSegments )
		m_pSegments[i]->DeleteDocument ( uDoc );
}


/// WARNING! static buffer, non-reenterable
static const char * FormatMicrotime ( int64_t uTime )
{
	static char sBuf[32];
	snprintf ( sBuf, sizeof(sBuf), "%d.%03d", int(uTime/1000000), int((uTime%1000000)/1000) );
	return sBuf;
}


void RtIndex_t::DumpToDisk ( const char * sFilename )
{
	CSphString sName, sError;

	CSphWriter wrHits, wrDocs, wrDict, wrRows;
	sName.SetSprintf ( "%s.spp", sFilename ); wrHits.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spd", sFilename ); wrDocs.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spi", sFilename ); wrDict.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spa", sFilename ); wrRows.OpenFile ( sName.cstr(), sError );

	BYTE bDummy = 1;
	wrDict.PutBytes ( &bDummy, 1 );
	wrDocs.PutBytes ( &bDummy, 1 );
	wrHits.PutBytes ( &bDummy, 1 );

	int64_t tmStart = sphMicroTimer();

	while ( m_pSegments.GetLength()>1 )
	{
		m_pSegments.Sort ( CmpSegments_fn() );
		RtSegment_t * pSeg1 = m_pSegments.Pop();
		RtSegment_t * pSeg2 = m_pSegments.Pop();
		pSeg1->OptimizeKlist ();
		pSeg2->OptimizeKlist ();
		m_pSegments.Add ( MergeSegments ( pSeg1, pSeg2 ) );
		SafeDelete ( pSeg1 );
		SafeDelete ( pSeg2 );
	}
	RtSegment_t * pSeg = m_pSegments[0];

	int64_t tmMerged = sphMicroTimer() ;
	printf ( "final merge done in %s sec\n", FormatMicrotime ( tmMerged-tmStart ) );

	SphWordID_t uLastWord = 0;
	SphOffset_t uLastDocpos = 0;

	static const int WORDLIST_CHECKPOINT = 1024;
	int iWords = 0;

	struct Checkpoint_t
	{
		uint64_t m_uWord;
		uint64_t m_uOffset;
	};
	CSphVector<Checkpoint_t> dCheckpoints;

	RtWordReader_t tInWord ( pSeg );
	for ( ;; )
	{
		const RtWord_t * pWord = tInWord.UnzipWord();
		if ( !pWord )
			break;

		SphDocID_t uLastDoc = 0;
		SphOffset_t uLastHitpos = 0;

		if ( !iWords )
		{
			Checkpoint_t & tChk = dCheckpoints.Add ();
			tChk.m_uWord = pWord->m_uWordID;
			tChk.m_uOffset = wrDict.GetPos();
		}

		wrDict.ZipInt ( pWord->m_uWordID - uLastWord );
		wrDict.ZipOffset ( wrDocs.GetPos() - uLastDocpos );
		wrDict.ZipInt ( pWord->m_uDocs );
		wrDict.ZipInt ( pWord->m_uHits );

		uLastDocpos = wrDocs.GetPos();
		uLastWord = pWord->m_uWordID;

		RtDocReader_t tInDoc ( pSeg, *pWord );
		for ( ;; )
		{
			const RtDoc_t * pDoc = tInDoc.UnzipDoc();
			if ( !pDoc )
				break;

			wrDocs.ZipOffset ( pDoc->m_uDocID-uLastDoc );
			wrDocs.ZipOffset ( wrHits.GetPos() - uLastHitpos );
			wrDocs.ZipInt ( pDoc->m_uFields );
			wrDocs.ZipInt ( pDoc->m_uHits );
			uLastDoc = pDoc->m_uDocID;
			uLastHitpos = wrHits.GetPos();

			if ( pDoc->m_uHits>1 )
			{
				DWORD uLastHit = 0;

				RtHitReader_t tInHit ( pSeg, pDoc );
				for ( DWORD uValue=tInHit.UnzipHit(); uValue; uValue=tInHit.UnzipHit() )
				{
					wrHits.ZipInt ( uValue - uLastHit );
					uLastHit = uValue;
				}
			} else
			{
				wrHits.ZipInt ( pDoc->m_uHit );
			}
			wrHits.ZipInt ( 0 );
		}
		wrDocs.ZipInt ( 0 );

		if ( ++iWords==WORDLIST_CHECKPOINT )
		{
			wrDict.ZipInt ( 0 );
			wrDict.ZipOffset ( wrDocs.GetPos() - uLastDocpos ); // store last hitlist length

			uLastDocpos = 0;
			uLastWord = 0;

			iWords = 0;
		}
	}
	wrDict.ZipInt ( 0 ); // indicate checkpoint
	wrDict.ZipOffset ( wrDocs.GetPos() - uLastDocpos ); // store last doclist length

	if ( dCheckpoints.GetLength() )
		wrDict.PutBytes ( &dCheckpoints[0], dCheckpoints.GetLength()*sizeof(Checkpoint_t) );

	wrRows.PutBytes ( &pSeg->m_dRows[0], pSeg->m_dRows.GetLength()*sizeof(CSphRowitem) );

	wrHits.CloseFile ();
	wrDocs.CloseFile ();
	wrDict.CloseFile ();
	wrRows.CloseFile ();

	int64_t tmDump = sphMicroTimer ();
	printf ( "dump done in %s sec\n", FormatMicrotime ( tmDump-tmMerged ) );
}

//////////////////////////////////////////////////////////////////////////
// SEARCHING
//////////////////////////////////////////////////////////////////////////

struct RtQword_t : public ISphQword
{
	friend struct RtIndex_t;

protected:
	RtDocReader_t *		m_pDocReader;
	CSphMatch			m_tMatch;

	DWORD				m_uNextHit;
	RtHitReader2_t		m_tHitReader;

	RtSegment_t *		m_pSeg;

public:
	RtQword_t ()
		: m_pDocReader ( NULL )
		, m_uNextHit ( 0 )
		, m_pSeg ( NULL )
	{
		m_tMatch.Reset ( 0 );
	}

	virtual const CSphMatch & GetNextDoc ( DWORD * )
	{
		for ( ;; )
		{
			const RtDoc_t * pDoc = m_pDocReader->UnzipDoc();
			if ( !pDoc )
			{
				m_tMatch.m_iDocID = 0;
				return m_tMatch;
			}

			if ( m_pSeg->m_dKlist.BinarySearch ( pDoc->m_uDocID ) )
				continue;
			if ( m_pSeg->m_dKlistAccum.Contains ( pDoc->m_uDocID ) )
				continue;

			m_tMatch.m_iDocID = pDoc->m_uDocID;
			m_uFields = pDoc->m_uFields;
			m_uMatchHits = pDoc->m_uHits;
			m_iHitlistPos = (uint64_t(pDoc->m_uHits)<<32) + pDoc->m_uHit;
			return m_tMatch;
		}
	}

	virtual void SeekHitlist ( SphOffset_t uOff )
	{
		int iHits = int(uOff>>32);
		if ( iHits==1 )
		{
			m_uNextHit = DWORD(uOff);
		} else
		{
			m_uNextHit = 0;
			m_tHitReader.Seek ( DWORD(uOff), iHits );
		}
	}

	virtual DWORD GetNextHit ()
	{
		if ( m_uNextHit==0 )
		{
			return m_tHitReader.UnzipHit();

		} else if ( m_uNextHit==0xffffffffUL )
		{
			return 0;

		} else
		{
			DWORD uRes = m_uNextHit;
			m_uNextHit = 0xffffffffUL;
			return uRes;
		}
	}
};


struct RtQwordSetup_t : ISphQwordSetup
{
	RtSegment_t * m_pSeg;
};


ISphQword * RtIndex_t::QwordSpawn () const
{
	return new RtQword_t ();
}


bool RtIndex_t::QwordSetup ( ISphQword * pQword, const ISphQwordSetup * pSetup ) const
{
	RtQword_t * pMyWord = dynamic_cast<RtQword_t*> ( pQword );
	if ( !pMyWord )
		return false;

	const RtQwordSetup_t * pMySetup = dynamic_cast<const RtQwordSetup_t*> ( pSetup );
	if ( !pMySetup )
		return false;

	RtWordReader_t tReader ( pMySetup->m_pSeg );
	for ( ;; )
	{
		const RtWord_t * pWord = tReader.UnzipWord();
		if ( !pWord )
			break;

		if ( pWord->m_uWordID==pMyWord->m_iWordID )
		{
			pMyWord->m_iDocs = pWord->m_uDocs;
			pMyWord->m_iHits = pWord->m_uHits;

			SafeDelete ( pMyWord->m_pDocReader );
			pMyWord->m_pDocReader = new RtDocReader_t ( pMySetup->m_pSeg, *pWord );

			pMyWord->m_tHitReader.m_pBase = NULL;
			if ( pMySetup->m_pSeg->m_dHits.GetLength() )
				pMyWord->m_tHitReader.m_pBase = &pMySetup->m_pSeg->m_dHits[0];

			pMyWord->m_pSeg = pMySetup->m_pSeg;
			return true;
		}
	}

	return false;
}


bool RtIndex_t::EarlyReject ( CSphMatch & tMatch ) const
{
	/*!COMMIT*/
	return false;
}


void RtIndex_t::CopyDocinfo ( CSphMatch & tMatch, const DWORD * pFound ) const
{
	if ( !pFound )
		return;

	// copy from storage
	assert ( tMatch.m_pRowitems );
	assert ( DOCINFO2ID(pFound)==tMatch.m_iDocID );
	memcpy ( tMatch.m_pRowitems, DOCINFO2ATTRS(pFound), m_tSchema.GetRowSize()*sizeof(CSphRowitem) );
}


const CSphRowitem * RtIndex_t::FindDocinfo ( const RtSegment_t * pSeg, SphDocID_t uDocID ) const
{
	// FIXME! move to CSphIndex, and implement hashing
	if ( pSeg->m_dRows.GetLength()==0 )
		return NULL;

	int iStride = m_iStride;
	int iStart = 0;
	int iEnd = pSeg->m_iRows;
	assert ( iStride==( DOCINFO_IDSIZE + m_tSchema.GetRowSize() ) );

	const CSphRowitem * pStorage = &pSeg->m_dRows[0];
	const CSphRowitem * pFound = NULL;

	if ( uDocID==DOCINFO2ID ( &pStorage [ iStart*iStride ] ) )
	{
		pFound = &pStorage [ iStart*iStride ];

	} else if ( uDocID==DOCINFO2ID ( &pStorage [ iEnd*iStride ] ) )
	{
		pFound = &pStorage [ iEnd*iStride ];

	} else
	{
		while ( iEnd-iStart>1 )
		{
			// check if nothing found
			if (
				uDocID < DOCINFO2ID ( &pStorage [ iStart*iStride ] ) ||
				uDocID > DOCINFO2ID ( &pStorage [ iEnd*iStride ] ) )
				break;
			assert ( uDocID > DOCINFO2ID ( &pStorage [ iStart*iStride ] ) );
			assert ( uDocID < DOCINFO2ID ( &pStorage [ iEnd*iStride ] ) );

			int iMid = iStart + (iEnd-iStart)/2;
			if ( uDocID==DOCINFO2ID ( &pStorage [ iMid*iStride ] ) )
			{
				pFound = &pStorage [ iMid*iStride ];
				break;
			}
			if ( uDocID<DOCINFO2ID ( &pStorage [ iMid*iStride ] ) )
				iEnd = iMid;
			else
				iStart = iMid;
		}
	}

	return pFound;
}


bool RtIndex_t::MultiQuery ( CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters, ISphMatchSorter ** ppSorters )
{
	RLOCK ( this );

	/*!COMMIT*/
	assert ( pQuery );
	assert ( pResult );
	assert ( ppSorters );

	// empty index, empty result
	if ( !m_pSegments.GetLength() )
	{
		pResult->m_iQueryTime = 0;
		return true;
	}

	// start counting
	pResult->m_iQueryTime = 0;
	int64_t tmQueryStart = sphMicroTimer();

	// setup search terms
	RtQwordSetup_t tTermSetup;
	tTermSetup.m_pDict = m_pDict;
	tTermSetup.m_pIndex = this;
	tTermSetup.m_eDocinfo = m_tSettings.m_eDocinfo;
	tTermSetup.m_tMin.m_iRowitems = m_tSchema.GetRowSize();
	tTermSetup.m_iToCalc = 0; /*!COMMIT pResult->m_tSchema.GetRowSize() - m_tSchema.GetRowSize(); */
	if ( pQuery->m_uMaxQueryMsec>0 )
		tTermSetup.m_iMaxTimer = sphMicroTimer() + pQuery->m_uMaxQueryMsec*1000; // max_query_time
	tTermSetup.m_pWarning = &pResult->m_sWarning;
	tTermSetup.m_pSeg = m_pSegments[0];

	// bind weights
	BindWeights ( pQuery );

	// setup query
	// must happen before index-level reject, in order to build proper keyword stats
	CSphScopedPtr<ISphRanker> pRanker ( sphCreateRanker ( pQuery, pQuery->m_sQuery.cstr(), pResult, tTermSetup, m_sLastError ) );
	if ( !pRanker.Ptr() )
		return false;

	ARRAY_FOREACH ( iSeg, m_pSegments )
	{
		if ( iSeg!=0 )
		{
			tTermSetup.m_pSeg = m_pSegments[iSeg];
			pRanker->Reset ( tTermSetup );
		}

		CSphMatch * pMatch = pRanker->GetMatchesBuffer();
		for ( ;; )
		{
			int iMatches = pRanker->GetMatches ( m_iWeights, m_dWeights );
			if ( iMatches<=0 )
				break;
			for ( int i=0; i<iMatches; i++ )
			{
				CopyDocinfo ( pMatch[i], FindDocinfo ( m_pSegments[iSeg], pMatch[i].m_iDocID ) );
				for ( int iSorter=0; iSorter<iSorters; iSorter++ )
					ppSorters[iSorter]->Push ( pMatch[i] );
			}
		}
	}

	// query timer
	pResult->m_iQueryTime = int( ( sphMicroTimer()-tmQueryStart )/1000 );
	return true;
}


bool RtIndex_t::GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, bool bGetStats )
{
	/*!COMMIT*/
	return false;
}

//////////////////////////////////////////////////////////////////////////

// FIXME! move to CSphIndex
CSphQueryResult * RtIndex_t::Query ( CSphQuery * pQuery )
{
	// create sorter
	CSphString sError;
	ISphMatchSorter * pTop = sphCreateQueue ( pQuery, m_tSchema, sError );
	if ( !pTop )
	{
		m_sLastError.SetSprintf ( "failed to create sorting queue: %s", sError.cstr() );
		return NULL;
	}

	// create result
	CSphQueryResult * pResult = new CSphQueryResult();

	// run query
	if ( QueryEx ( pQuery, pResult, pTop ) )
	{
		// convert results and return
		pResult->m_dMatches.Reset ();
		sphFlattenQueue ( pTop, pResult, 0 );
	} else
	{
		SafeDelete ( pResult );
	}

	SafeDelete ( pTop );
	return pResult;
}


// FIXME! move to CSphIndex
bool RtIndex_t::QueryEx ( CSphQuery * pQuery, CSphQueryResult * pResult, ISphMatchSorter * pTop )
{
	bool bRes = MultiQuery ( pQuery, pResult, 1, &pTop );
	pResult->m_iTotalMatches += bRes ? pTop->GetTotalCount () : 0;
	pResult->m_tSchema = pTop->GetOutgoingSchema();
	return bRes;
}


// FIXME! move to CSphIndex
void RtIndex_t::BindWeights ( const CSphQuery * pQuery )
{
	const int MIN_WEIGHT = 1;

	// defaults
	m_iWeights = Min ( m_tSchema.m_dFields.GetLength(), SPH_MAX_FIELDS );
	for ( int i=0; i<m_iWeights; i++ )
		m_dWeights[i] = MIN_WEIGHT;

	// name-bound weights
	if ( pQuery->m_dFieldWeights.GetLength() )
	{
		ARRAY_FOREACH ( i, pQuery->m_dFieldWeights )
		{
			int j = m_tSchema.GetFieldIndex ( pQuery->m_dFieldWeights[i].m_sName.cstr() );
			if ( j>=0 && j<SPH_MAX_FIELDS )
				m_dWeights[j] = Max ( MIN_WEIGHT, pQuery->m_dFieldWeights[i].m_iValue );
		}
		return;
	}

	// order-bound weights
	if ( pQuery->m_pWeights )
	{
		for ( int i=0; i<Min ( m_iWeights, pQuery->m_iWeights ); i++ )
			m_dWeights[i] = Max ( MIN_WEIGHT, (int)pQuery->m_pWeights[i] );
	}
}

//////////////////////////////////////////////////////////////////////////

ISphRtIndex * sphCreateIndexRT ( const CSphSchema & tSchema )
{
	return new RtIndex_t ( tSchema );
}

//
// $Id$
//
