// Copyright 2019 <>< Charles Lohr licensable under the MIT/X11 or NewBSD licenses.

#include <cnovrparts.h>
#include <cnovr.h>
#include <cnovrutil.h>
#include <chew.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stb_image.h>


static void CNOVRRenderFrameBufferDelete( cnovr_rf_buffer * ths )
{
	if( ths->nRenderFramebufferId ) glDeleteFramebuffers( 1, &ths->nRenderFramebufferId );
	if( ths->nResolveFramebufferId ) glDeleteFramebuffers( 1, &ths->nResolveFramebufferId );
	if( ths->nResolveTextureId ) glDeleteTextures( 1, &ths->nResolveTextureId );
	if( ths->nDepthBufferId ) glDeleteRenderbuffers( 1, &ths->nDepthBufferId );
	if( ths->nRenderTextureId ) glDeleteTextures( 1, &ths->nRenderTextureId );

	free( ths );
}

cnovr_rf_buffer * CNOVRRFBufferCreate( int nWidth, int nHeight, int multisample )
{
	cnovr_rf_buffer * ret = malloc( sizeof( cnovr_rf_buffer ) );
	memset( ret, 0, sizeof( *ret ) );
	ret->header.Delete = (cnovrfn)CNOVRRenderFrameBufferDelete;
	ret->header.Type = 1;

	glGenFramebuffers(1, &ret->nRenderFramebufferId );
	glBindFramebuffer(GL_FRAMEBUFFER, ret->nRenderFramebufferId);

	glGenRenderbuffers(1, &ret->nDepthBufferId);
	glBindRenderbuffer(GL_RENDERBUFFER, ret->nDepthBufferId);
	if( multisample )
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, multisample, GL_DEPTH_COMPONENT, nWidth, nHeight );
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,	ret->nDepthBufferId );

	glGenTextures(1, &ret->nRenderTextureId );
	glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ret->nRenderTextureId );
	if( multisample )
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, multisample, GL_RGBA8, nWidth, nHeight, 1);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, ret->nRenderTextureId, 0);

	glGenFramebuffers(1, &ret->nResolveFramebufferId );
	glBindFramebuffer(GL_FRAMEBUFFER, ret->nResolveFramebufferId);

	glGenTextures(1, &ret->nResolveTextureId );
	glBindTexture(GL_TEXTURE_2D, ret->nResolveTextureId );
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nWidth, nHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ret->nResolveTextureId, 0);

	// check FBO status
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		CNOVRRenderFrameBufferDelete( ret );
		return 0;
	}

	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	return ret;
}


void CNOVRFBufferActivate( cnovr_rf_buffer * b )
{
	b->origw = cnovrstate->iRTWidth;
	b->origh = cnovrstate->iRTHeight;
	int w = cnovrstate->iRTWidth = b->width;
	int h = cnovrstate->iRTHeight = b->height;
	glBindFramebuffer( GL_FRAMEBUFFER, b->nResolveFramebufferId );
 	glViewport(0, 0, w, h );
}

void CNOVRFBufferDeactivate( cnovr_rf_buffer * b )
{
 	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	int w = cnovrstate->iRTWidth = b->origw;
	int h = cnovrstate->iRTHeight = b->origh;
 	glViewport(0, 0, w, h );
}

// Just FYI to blit a buffer:
//	glBindFramebuffer(GL_READ_FRAMEBUFFER, leftEyeDesc.m_nRenderFramebufferId);
//	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, leftEyeDesc.m_nResolveFramebufferId );
//	glBlitFramebuffer( 0, 0, m_nRenderWidth, m_nRenderHeight, 0, 0, m_nRenderWidth, m_nRenderHeight, 
//		GL_COLOR_BUFFER_BIT,
// 		GL_LINEAR );
// 	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
//	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0 );	



//////////////////////////////////////////////////////////////////////////////

static void CNOVRShaderDelete( cnovr_shader * ths )
{
	if( ths->nShaderID ) glDeleteProgram( ths->nShaderID );
	FileTimeRemoveTagged( ths );
	free( ths->shaderfilebase );
	free( ths );
}

static GLuint CNOVRShaderCompilePart( GLuint shader_type, const char * shadername, const char * compstr )
{
	GLuint nShader = glCreateShader( shader_type );
	glShaderSource( nShader, 1, &compstr, NULL );
	glCompileShader( nShader );
	GLint vShaderCompiled = GL_FALSE;

	glGetShaderiv( nShader, GL_COMPILE_STATUS, &vShaderCompiled );
	if ( vShaderCompiled != GL_TRUE )
	{
		CNOVRAlert( cnovrstate->pCurrentModel, 1, "Unable to compile shader: %s\n", shadername );
		int retval;
		glGetShaderiv( nShader, GL_INFO_LOG_LENGTH, &retval );
		if ( retval > 1 ) {
			char * log = (char*)malloc( retval );
			glGetShaderInfoLog( nShader, retval, NULL, log );
			CNOVRAlert( cnovrstate->pCurrentModel, 1, "%s\n", log );
			free( log );
		}

		glDeleteShader( nShader );
		return 0;
	}
	return nShader;
}

//XXX TODO: Cleanup reload mechanism.
static void CNOVRShaderPrerender( cnovr_shader * ths )
{
	if( ths->bChangeFlag )
	{
		//Re-load shader

		//Careful: Need to re-try in case a program is still writing.
		if( ths->bChangeFlag++ > 2 )
		{
			ths->bChangeFlag = 0;
			return;
		}
		
		GLuint nGeoShader  = 0;
		GLuint nFragShader = 0;
		GLuint nVertShader = 0;

		char stfbGeo[CNOVR_MAX_PATH];
		char stfbFrag[CNOVR_MAX_PATH];
		char stfbVert[CNOVR_MAX_PATH];
		const char * filedataGeo = 0;
		const char * filedataFrag = 0;
		const char * filedataVert = 0;

		sprintf( stfbGeo, "%s.geo", ths->shaderfilebase );
		filedataGeo = FileToString( stfbGeo, 0 );
		sprintf( stfbFrag, "%s.frag", ths->shaderfilebase );
		filedataFrag = FileToString( stfbFrag, 0 );
		sprintf( stfbVert, "%s.vert", ths->shaderfilebase );
		filedataVert = FileToString( stfbVert, 0 );

		if( !filedataFrag || !filedataVert )
		{
			CNOVRAlert( cnovrstate->pCurrentModel, 1, "Unable to open vert/frag in shader: %s\n", ths->shaderfilebase );
			return;
		}

		if( filedataGeo )
		{
			nGeoShader = CNOVRShaderCompilePart( GL_GEOMETRY_SHADER, stfbGeo, filedataGeo );
		}
		nFragShader = CNOVRShaderCompilePart( GL_FRAGMENT_SHADER, stfbFrag, filedataFrag );
		nVertShader = CNOVRShaderCompilePart( GL_VERTEX_SHADER, stfbVert, filedataVert );

		bool compfail = false;
		if( filedataGeo )
		{
			if ( !nGeoShader )
				compfail = true;
		}
		if ( !nVertShader || !nFragShader )
		{
			compfail = true;
		}

		GLuint unProgramID = 0;

		if ( !compfail )
		{
			unProgramID = glCreateProgram();
			if( filedataGeo )
			{
				glAttachShader( unProgramID, nGeoShader );
			}
			glAttachShader( unProgramID, nFragShader );
			glAttachShader( unProgramID, nVertShader );
			glLinkProgram( unProgramID );

			GLint programSuccess = GL_TRUE;
			glGetProgramiv( unProgramID, GL_LINK_STATUS, &programSuccess );
			if ( programSuccess != GL_TRUE )
			{
				CNOVRAlert( cnovrstate->pCurrentModel, 1, "Shader linking failed: %s\n", ths->shaderfilebase );
				int retval;
				glGetShaderiv( unProgramID, GL_INFO_LOG_LENGTH, &retval );
				if ( retval > 1 ) {
					char * log = (char*)malloc( retval );
					glGetProgramInfoLog( unProgramID, retval, NULL, log );
					CNOVRAlert( cnovrstate->pCurrentModel, 1, "%s\n", log );
					free( log );
				}
				glDeleteProgram( unProgramID );
				compfail = true;
			}
		}
		else
		{
			CNOVRAlert( cnovrstate->pCurrentModel, 1, "Shader compilation failed: %s\n", ths->shaderfilebase );
		}

		if ( unProgramID )
		{
			if ( ths->nShaderID )
			{
				ths->bChangeFlag = 0;
				glDeleteProgram( ths->nShaderID );
			}
			ths->nShaderID = unProgramID;
		}

		if( nGeoShader )  glDeleteShader( nGeoShader );
		if( nFragShader ) glDeleteShader( nFragShader );
		if( nVertShader ) glDeleteShader( nVertShader );
	}
}

static void CNOVRShaderRender( cnovr_shader * ths )
{
	int shdid = ths->nShaderID;
	if( !shdid ) return;
	glUseProgram( shdid );
	glUniform4f( 16, cnovrstate->iRTWidth, cnovrstate->iRTHeight, cnovrstate->fFar, cnovrstate->fNear );
	glUniformMatrix4fv( 17, 1, 0, cnovrstate->mPerspective ); 
	glUniformMatrix4fv( 18, 1, 0, cnovrstate->mView ); 
	glUniformMatrix4fv( 19, 1, 0, cnovrstate->mModel ); 
};

cnovr_shader * CNOVRShaderCreate( const char * shaderfilebase )
{
	cnovr_shader * ret = malloc( sizeof( cnovr_shader ) );
	memset( ret, 0, sizeof( *ret ) );
	ret->header.Delete = (cnovrfn)CNOVRShaderDelete;
	ret->header.Render = (cnovrfn)CNOVRShaderRender;
	ret->header.Prerender = (cnovrfn)CNOVRShaderPrerender;
	ret->header.Type = TYPE_SHADER;
	ret->shaderfilebase = strdup( shaderfilebase );

	char stfb[CNOVR_MAX_PATH];
	sprintf( stfb, "%s.geo", shaderfilebase );
	FileTimeAddWatch( stfb, &ret->bChangeFlag, ret );
	sprintf( stfb, "%s.frag", shaderfilebase );
	FileTimeAddWatch( stfb, &ret->bChangeFlag, ret );
	sprintf( stfb, "%s.vert", shaderfilebase );
	FileTimeAddWatch( stfb, &ret->bChangeFlag, ret );

	ret->bChangeFlag = 1;
}

//////////////////////////////////////////////////////////////////////////////

static void CNOVRTextureLoadFileTask( void * opaquev, int opaquei )
{
	cnovr_texture * t = (cnovr_texture*)opaquev;
	OGLockMutex( t->mutProtect );
	char * localfn = strdup( t->texfile );
	OGUnlockMutex( t->mutProtect );

	int x, y, chan;
	stbi_uc * data = stbi_load( localfn, &x, &y, &chan, 4 );
	free( localfn );

	if( data )
	{
		CNOVRTextureLoadDataAsync( t, x, y, chan, 0, data );
		t->bLoading = 0;
	}

}

static void CNOVRTextureUploadCallback( void * vths, int i )
{
	cnovr_texture * t = (cnovr_texture*)vths;
	OGLockMutex( t->mutProtect );
	t->bTaintData = 0;
	glBindTexture( GL_TEXTURE_2D, t->nTextureId );
	glTexImage2D( GL_TEXTURE_2D,
		0,
		t->nInternalFormat,
		t->width,
		t->height,
		0,
		t->nFormat,
		t->nType,
		t->data );
	glBindTexture( GL_TEXTURE_2D, 0 );
	OGUnlockMutex( t->mutProtect );
}

static void CNOVRTextureDelete( cnovr_texture * ths )
{
	OGLockMutex( ths->mutProtect );
	//In case any file changes are being watched.
	FileTimeRemoveTagged( ths );

	//Pre-emptively kill any possible pending queued events.
	CNOVRJobCancel( cnovrQAsync, CNOVRTextureLoadFileTask, ths, 0 );
	CNOVRJobCancel( cnovrQPrerender, CNOVRTextureUploadCallback, ths, 0 );

	if( ths->nTextureId )
	{
		glDeleteTextures( 1, &ths->nTextureId );
	}

	if( ths->data ) free( ths->data );
	if( ths->texfile ) free( ths->texfile );
	OGDeleteMutex( ths->mutProtect );
	free( ths );
}

static void CNOVRTextureRender( cnovr_texture * ths )
{
	glBindTexture( GL_TEXTURE_2D, ths->nTextureId );
}

//Defaults to a 1x1 px texture.
cnovr_texture * CNOVRTextureCreate( int initw, int inith, int initchan )
{
	cnovr_texture * ret = malloc( sizeof( cnovr_rf_buffer ) );
	memset( ret, 0, sizeof( *ret ) );
	ret->header.Delete = (cnovrfn)CNOVRTextureDelete;
	ret->header.Render = (cnovrfn)CNOVRTextureRender;
	ret->header.Type = TYPE_TEXTURE;
	ret->texfile = 0;

	ret->mutProtect = OGCreateMutex();

	glGenTextures( 1, &ret->nTextureId );

	ret->width = 1;
	ret->height = 1;
	ret->channels = 4;
	ret->data = malloc( 4 );
	ret->bTaintData = 0;
	ret->bLoading = 0;
	ret->bFileChangeFlag = 0;
	memset( ret->data, 255, 4 );

	glBindTexture( GL_TEXTURE_2D, ret->nTextureId );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );

	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, ret->width, ret->height,
		0, GL_RGBA, GL_UNSIGNED_BYTE, ret->data );

	glBindTexture( GL_TEXTURE_2D, 0 );

	return ret;
}

int CNOVRTextureLoadFileAsync( cnovr_texture * tex, const char * texfile )
{
	OGLockMutex( tex->mutProtect );
	if( tex->texfile ) free( tex->texfile );
	tex->texfile = strdup( texfile );
	tex->bLoading = 1;
	CNOVRJobCancel( cnovrQAsync, CNOVRTextureLoadFileTask, tex, 0 ); //Just in case.
	CNOVRJobTack( cnovrQAsync, CNOVRTextureLoadFileTask, tex, 0 );
	OGUnlockMutex( tex->mutProtect );
}

int CNOVRTextureLoadDataAsync( cnovr_texture * tex, int w, int h, int chan, int is_float, void * data )
{
	OGLockMutex( tex->mutProtect );

	if( tex->data ) free( tex->data );
	tex->data = data;
	tex->width = w;
	tex->height = h;
	tex->channels = chan;

	static const int channelmapF[] = { 0, GL_R32F, GL_RG32F, GL_RGB32F, GL_RGBA32F };
	static const int channelmapI[] = { 0, GL_R8, GL_RG8, GL_RGB8, GL_RGBA8 };
	static const int channelmapB[] = { 0, GL_RED, GL_RG, GL_RGB, GL_RGBA };

	if( is_float )
		tex->nInternalFormat = channelmapF[chan];
	else
		tex->nInternalFormat = channelmapI[chan];
	tex->nFormat = channelmapB[chan];
	tex->nType = is_float?GL_FLOAT:GL_UNSIGNED_BYTE;
	tex->bTaintData = 1;
	CNOVRJobTack( cnovrQPrerender, CNOVRTextureUploadCallback, tex, 0 );
	OGUnlockMutex( tex->mutProtect );
}



///////////////////////////////////////////////////////////////////////////////

cnovr_vbo * CNOVRCreateVBO( int iStride, int bDynamic, int iInitialSize, int iAttribNo )
{
	cnovr_vbo * ret = malloc( sizeof( cnovr_vbo ) );
	memset( ret, 0, sizeof( cnovr_vbo ) );
	ret->iVertexCount = iInitialSize;

	if( iInitialSize < 1 ) iInitialSize = 1;
	ret->pVertices = malloc( iInitialSize * sizeof(float) * iStride );
	ret->iStride = iStride;

	glGenBuffers( 1, &ret->nVBO );
	ret->bDynamic = bDynamic;
	ret->mutData = OGCreateMutex();

	glBindBuffer( GL_ARRAY_BUFFER, ret->nVBO);
	glEnableClientState( GL_VERTEX_ARRAY);
	glEnableVertexAttribArray( iAttribNo );
	glVertexPointer( iStride, GL_FLOAT, 0, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	return ret;
}


void CNOVRVBOTackv( cnovr_vbo * g, int nverts, float * v )
{
	OGLockMutex( g->mutData );
	int stride = g->iStride;
	g->pVertices = realloc( g->pVertices, (g->iVertexCount+1)*stride*sizeof(float) );
	float * verts = g->pVertices + (g->iVertexCount*stride);
	int tocopy = nverts;
	if( nverts > stride ) tocopy = stride;
	int i;
	for( i = 0; i < tocopy; i++ )
	{
		verts[i] = v[i];
	}
	for( ; i < stride; i++ )
	{
		verts[i] = 0;
	}
	g->iVertexCount++;
	OGUnlockMutex( g->mutData );
	CNOVRVBOTaint( g );
}

void CNOVRVBOTack( cnovr_vbo * g,  int nverts, ... )
{
	OGLockMutex( g->mutData );
	int stride = g->iStride;
	g->pVertices = realloc( g->pVertices, (g->iVertexCount+1)*stride*sizeof(float) );
	float * verts = g->pVertices;
	va_list argp;
	va_start( argp, nverts );
	int i;
	for( i = 0; i < stride; i++ )
	{
		verts[i] = va_arg(argp, double);
	}
	g->pVertices++;
	va_end( argp );
	OGUnlockMutex( g->mutData );
	CNOVRVBOTaint( g );
}

static void CNOVRVBOPerformUpload( void * gv, int opaquei )
{
	cnovr_vbo * g = (cnovr_vbo *)gv;

	OGLockMutex( g->mutData );

	//This happens from within the render thread
	glBindBuffer( GL_ARRAY_BUFFER, g->nVBO );

	//XXX TODO: Consider streaming the data.
	glBufferData( GL_ARRAY_BUFFER, g->iStride*sizeof(float)*g->iVertexCount, g->pVertices, g->bDynamic?GL_DYNAMIC_DRAW:GL_STATIC_DRAW);
	glVertexPointer( g->iStride, GL_FLOAT, 0, 0);

	OGUnlockMutex( g->mutData );
}

void CNOVRVBOTaint( cnovr_vbo * g )
{
	CNOVRJobCancel( cnovrQPrerender, CNOVRVBOPerformUpload, (void*)g, 0 );
	CNOVRJobTack( cnovrQPrerender, CNOVRVBOPerformUpload, (void*)g, 0 );
}

void CNOVRVBODelete( cnovr_vbo * g )
{
	OGLockMutex( g->mutData );
	CNOVRJobCancel( cnovrQPrerender, CNOVRVBOPerformUpload, (void*)g, 0 );
	glDeleteBuffers( 1, &g->nVBO );
	free( g->pVertices );
	OGDeleteMutex( g->mutData );
}

void CNOVRVBOSetStride( cnovr_vbo * g, int stride )
{
	g->iStride = stride;
	CNOVRVBOTaint( g );
}

///////////////////////////////////////////////////////////////////////////////

static void CNOVRModelUpdateIBO( void * vm, int i )
{
	cnovr_model * m = (cnovr_model *)vm;
	OGLockMutex( m->model_mutex );
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->nIBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(m->pIndices[0])*m->iIndexCount, m->pIndices, GL_STATIC_DRAW);
	OGUnlockMutex( m->model_mutex );
}

void CNOVRModelTaintIndices( cnovr_model * vm )
{
	CNOVRJobTack( cnovrQPrerender, CNOVRModelUpdateIBO, (void*)vm, 0 );	
}

static int CNOVRModelDelete( cnovr_model * m )
{
	OGLockMutex( m->model_mutex );
	CNOVRJobCancel( cnovrQPrerender, CNOVRModelUpdateIBO, (void*)m, 0 );
	int i;
	for( i = 0; i < m->iGeos; i++ )
	{
		CNOVRVBODelete( m->pGeos[i] );
	}
	for( i = 0; i < m->nMeshes; i++ )
	{
		if( m->sMeshMarks && m->sMeshMarks[i] ) free( m->sMeshMarks[i] );
	}
	free( m->pIndices );
	if( m->geofile ) free( m->geofile );
	glDeleteBuffers( 1, &m->nIBO );
}


static int CNOVRModelRender( cnovr_model * m )
{
	static cnovr_model * last_rendered_model = 0;

	//XXX Tricky: Don't lock model, so if we're loading while rendering, we don't hitch.
	if( m != last_rendered_model )
	{
		//Try binding any textures.
		int i;
		int count = m->iTextures;
		cnovr_texture ** ts = m->pTextures;
		for( i = 0; i < count; i++ )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			ts[i]->header.Render( ts[i] );
		}

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->nIBO );
		count = m->iGeos;
		for( i = 0; i < count; i++ )
		{
			glBindBuffer( GL_ARRAY_BUFFER, m->pGeos[i]->nVBO );
			glEnableVertexAttribArray(i);
		}
	}
	glDrawElements( GL_TRIANGLES, m->iIndexCount, GL_UNSIGNED_INT, 0 );
}

cnovr_model * CNOVRModelCreate( int initial_indices, int num_vbos, int rendertype )
{
	cnovr_model * ret = malloc( sizeof( cnovr_model ) );
	memset( ret, 0, sizeof( cnovr_model ) );
	ret->header.Delete = (cnovrfn)CNOVRModelDelete;
	ret->header.Render = (cnovrfn)CNOVRModelRender;
	ret->header.Type = TYPE_GEOMETRY;
	glGenBuffers( 1, &ret->nIBO );
	ret->iIndexCount = initial_indices;
	if( initial_indices < 1 ) initial_indices = 1;
	ret->pIndices = malloc( initial_indices * sizeof( GLuint ) );
	ret->nRenderType = rendertype;
	ret->iMeshMarks = malloc( sizeof( int ) );
	ret->iMeshMarks[0] = 0;
	ret->nMeshes = 1;

	ret->pGeos = malloc( sizeof( cnovr_vbo * ) * 1 );
	ret->iGeos = 0;
	ret->pTextures = malloc( sizeof( cnovr_texture * ) );
	*ret->pTextures = 0;
	ret->iTextures = 0;
	ret->geofile = 0;

	ret->bIsLoading = 0;
	ret->iLastVertMark = 0;
	ret->model_mutex = OGCreateMutex(); 
}

void CNOVRModelSetNumVBOsWithStrides( cnovr_model * m, int vbos, ... )
{
	int i;
	va_list argp;
	va_start( argp, vbos );

	for( i = 0; i < m->iGeos; i++ )
	{
		CNOVRVBODelete( m->pGeos[i] );
	}
	m->pGeos = realloc( m->pGeos, sizeof( cnovr_vbo * ) * vbos );
	m->iGeos = vbos;

	for( i = 0; i < vbos; i++ )
	{
		m->pGeos[i] = CNOVRCreateVBO( va_arg(argp, int), 0, 0, i );
	}
	va_end( argp );
}

void CNOVRModelSetNumVBOs( cnovr_model * m, int vbos )
{
	int i;
	for( i = 0; i < m->iGeos; i++ )
	{
		CNOVRVBODelete( m->pGeos[i] );
	}
	m->pGeos = realloc( m->pGeos, sizeof( cnovr_vbo * ) * vbos );
	m->iGeos = vbos;

	for( i = 0; i < vbos; i++ )
	{
		m->pGeos[i] = CNOVRCreateVBO( 4, 0, 0, i );
	}
}

void CNOVRModelSetNumIndices( cnovr_model * m, uint32_t indices )
{
	m->iIndexCount = indices;
	m->pIndices = realloc( m->pIndices, sizeof( GLuint ) * indices );
}

void CNOVRModelResetMarks( cnovr_model * m )
{
	int i;
	for( i = 0; i < m->nMeshes; i++ )
	{
		if( m->sMeshMarks && m->sMeshMarks[i] ) free( m->sMeshMarks[i] );
	}
	m->sMeshMarks = realloc( m->sMeshMarks, sizeof( char * ) );
	m->iMeshMarks = realloc( m->iMeshMarks, sizeof( uint32_t ) );
	m->nMeshes = 1;
	m->iMeshMarks[0] = 0;
	m->iLastVertMark = 0;
}

void CNOVRDelinateGeometry( cnovr_model * m, const char * sectionname )
{
	if( m->iIndexCount == 0 )
	{
		m->nMeshes = 1;
	}
	else
	{
		m->nMeshes++;
	}
	m->iMeshMarks = realloc( m->iMeshMarks, sizeof( uint32_t ) * m->nMeshes );
	m->sMeshMarks = realloc( m->sMeshMarks, sizeof( char * ) * m->nMeshes );
	m->iMeshMarks[m->nMeshes] = m->iIndexCount;
	m->sMeshMarks[m->nMeshes] = strdup( sectionname );
}


void CNOVRModelTackIndex( cnovr_model * m, int nindices, ...)
{
	int iIndexCount = m->iIndexCount;
	m->pIndices = realloc( m->pIndices, iIndexCount + nindices );
	uint32_t * pIndices = m->pIndices + iIndexCount;
	int i;
	va_list argp;
	va_start( argp, nindices );
	for( i = 0; i < nindices; i++ )
	{
		pIndices[i] = va_arg(argp, int);
	}
	va_end( argp );
	m->iIndexCount = m->iIndexCount + nindices;
}

void CNOVRModelTackIndexv( cnovr_model * m, int nindices, uint32_t * indices )
{
	int iIndexCount = m->iIndexCount;
	m->pIndices = realloc( m->pIndices, iIndexCount + nindices );
	uint32_t * pIndices = m->pIndices + iIndexCount;
	int i;
	for( i = 0; i < nindices; i++ )
	{
		pIndices[i] = indices[i];
	}
	m->iIndexCount = m->iIndexCount + nindices;
}

void CNOVRModelMakeCube( cnovr_model * m, float sx, float sy, float sz )
{
	//Bit pattern. 0 means -1, 1 means +1 on position.
	//n[face] n[+-](inverted)  v1[xyz] v2[xyz] v3[xyz];; TC can be computed from table based on N
	//XXX TODO: Check texture coord correctness.
	static const uint16_t trideets[] = {
		0b000000001011,
		0b000000011010,
		0b001111100110,
		0b001100111101,
		0b010101000100,
		0b010101001000,
		0b011111110010,
		0b011111010011,
		0b100110100000,
		0b100110000010,
		0b101011001101,
		0b101111011101,
	};
	OGLockMutex( m->model_mutex );
	if( m->iGeos != 3 )
	{
		CNOVRModelSetNumVBOsWithStrides( m, 3, 3, 4, 3 );
		CNOVRModelSetNumIndices( m, 0 );
		CNOVRModelResetMarks( m );
	}
	CNOVRDelinateGeometry( m, "cube" );

	int i;
	{
		uint32_t * indices = m->pIndices;
		for( i = 0; i < 36; i++ ) 
		{
			CNOVRModelTackIndex( m, i + m->iLastVertMark );
		}
	}

	{
		//float * points = m->pGeos[0]->pVertices;
		//float * texcoord = m->pGeos[1]->pVertices;
		//float * normals = m->pGeos[2]->pVertices;

		for( i = 0; i < 12; i++ )
		{
			uint16_t key = trideets[i];
			int normaxis = (key>>10)&3;
			float normplus = ((key>>9)&1)?1:-1;
			int tcaxis1 = (normaxis+1)%3;
			int tcaxis2 = (tcaxis1+1)%3;
			int vkeys[3] = { 
				(key >> 6)&7,
				(key >> 3)&7,
				(key >> 0)&7 };
			int j;
			for( j = 0; j < 3; j++ )
			{
				float stage[4];
				float staget[4];
				stage[0] = (vkeys[j]&4)?1:-1;
				stage[1] = (vkeys[j]&2)?1:-1;
				stage[2] = (vkeys[j]&1)?1:-1;
				stage[3] = 1;
				CNOVRVBOTackv( m->pGeos[0], 3, stage );

				staget[0] = stage[tcaxis1];
				staget[1] = stage[tcaxis2];
				staget[2] = 1;
				staget[3] = m->nMeshes;
				CNOVRVBOTackv( m->pGeos[1], 4, staget );

				stage[0] = (normaxis==0)?normplus:0;
				stage[1] = (normaxis==1)?normplus:0;
				stage[2] = (normaxis==2)?normplus:0;
				stage[3] = 0;
				CNOVRVBOTackv( m->pGeos[2], 3, stage );
			}
		}
	}

	m->iLastVertMark += 36;

	CNOVRVBOTaint( m->pGeos[0] );
	CNOVRVBOTaint( m->pGeos[1] );
	CNOVRVBOTaint( m->pGeos[2] );
	CNOVRModelTaintIndices( m );

	OGUnlockMutex( m->model_mutex );
}

void CNOVRModelMakeMesh( cnovr_model * m, int rows, int cols, float w, float h )
{
	//Copied from Spreadgine.
	int i;
	int x, y;
	int c = w * h;
	int v = (w+1)*(h+1);

	OGLockMutex( m->model_mutex );

	if( m->iGeos != 3 )
	{
		CNOVRModelSetNumVBOsWithStrides( m, 3, 3, 4, 3 );
		CNOVRModelSetNumIndices( m, 0 );
		CNOVRModelResetMarks( m );
	}
	CNOVRModelSetNumIndices( m, 6*c );

	CNOVRDelinateGeometry( m, "mesh" );

	{
		uint32_t * indices = m->pIndices;
		for( y = 0; y < h; y++ )
		for( x = 0; x < w; x++ )
		{
			int i = x + y * w;
			int k = m->iLastVertMark;
			CNOVRModelTackIndex( m, k + x + y * (w+1) );
			CNOVRModelTackIndex( m, k + (x+1) + y * (w+1) );
			CNOVRModelTackIndex( m, k + (x+1) + (y+1) * (w+1) );
			CNOVRModelTackIndex( m, k + (x) + (y) * (w+1) );
			CNOVRModelTackIndex( m, k + (x+1) + (y+1) * (w+1) );
			CNOVRModelTackIndex( m, k + (x) + (y+1) * (w+1) );
		}
	}

	{
		float stage[4];
		float stagen[3];

		stagen[0] = 0;
		stagen[1] = 0;
		stagen[2] = -1;

		for( y = 0; y <= h; y++ )
		for( x = 0; x <= w; x++ )
		{
			stage[0] = x/(float)w;
			stage[1] = y/(float)h;
			stage[2] = 1;
			stage[3] = m->nMeshes;

			CNOVRVBOTackv( m->pGeos[0], 3, stage );
			CNOVRVBOTackv( m->pGeos[1], 4, stage );
			CNOVRVBOTackv( m->pGeos[2], 3, stagen );
		}
	}

	m->iLastVertMark += (w+1)*(h+1);

	CNOVRVBOTaint( m->pGeos[0] );
	CNOVRVBOTaint( m->pGeos[1] );
	CNOVRVBOTaint( m->pGeos[2] );
	CNOVRModelTaintIndices( m );

	OGUnlockMutex( m->model_mutex );
}

void CNOVRModelApplyTextureFromFileAsync( cnovr_model * m, const char * sTextureFile )
{
	if( m->iTextures == 0 )
	{
		m->pTextures[0] = CNOVRTextureCreate( 1, 1, 4 );
		m->iTextures = 1;
	}
	CNOVRTextureLoadFileAsync( m->pTextures[0], sTextureFile );
}

void CNOVRModelRenderWithPose( cnovr_model * m, cnovr_pose * pose )
{
	pose_to_matrix44( cnovrstate->mModel, pose );
	glUniformMatrix4fv( UNIFORMSLOT_PERSPECTIVE, 1, 0, cnovrstate->mModel );
	m->header.Render( m );
}

int  CNOVRModelCollide( cnovr_model * m, const cnovr_point3d start, const cnovr_vec3d direction, cnovr_collide_results * r )
{
	if( m->iGeos == 0 ) return -1;
	int iMeshNo = 0;
	//Iterate through all this.
	float * vpos = m->pGeos[0]->pVertices;
	int stride = m->pGeos[0]->iStride;
	int i;
	for( i = 0; i < m->nMeshes; i++ )
	{
		int meshStart = m->iMeshMarks[i];
		int meshEnd = (i == m->nMeshes-1 ) ? m->iIndexCount : m->iMeshMarks[i+1];
		int j;
		for( j = meshStart; j < meshEnd; j+=3 )
		{
			int i0 = j+0;
			int i1 = j+1;
			int i2 = j+2;
			//i0..2 are the indices we will be verting.
			float * v0 = &vpos[i0*stride];
			float * v1 = &vpos[i1*stride];
			float * v2 = &vpos[i2*stride];

			float v10[3];
			float v21[3];
			float v02[3];
			float N[3];
			sub3d( v10, v1, v0 );
			sub3d( v21, v2, v1 );
			sub3d( v02, v0, v2 );
			{
				float v20[3];
				cross3d( N, v20, v10 );
				sub3d( v20, v2, v0 );
			}
			normalize3d( N, N );
			float D = dot3d(N, v0);
			float t = -( dot3d( N, start ) + D) / dot3d( N, direction ); 
			float Phit[3];
			scale3d( Phit, direction, t );
			add3d( Phit, Phit, start );
			float C0[3];
			float C1[3];
			float C2[3];
			sub3d( C0, Phit, v0 );
			sub3d( C1, Phit, v1 );
			sub3d( C2, Phit, v2 );
			cross3d( C0, v10, C0 );
			cross3d( C1, v21, C1 );
			cross3d( C2, v02, C2 );
			if( dot3d( N, C0 ) < 0 ||
				dot3d( N, C1 ) < 0 ||
				dot3d( N, C2 ) < 0 ) continue;

			//Else: We have a hit.
			if( t < r->t )
			{
				r->t = t;
				r->whichmesh = i;
				r->whichvert = j;
				copy3d( r->collidepos, Phit );

				//Now, do barycentric coordinates to get the rest of the info here.
				//XXX TODO TODO.
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
//Model Loaders

#define OBJBUFFERSIZE 256

///
/// OBJ File Loader (From Spreadgine)
///

#define TBUFFERSIZE 256
#define VBUFFERSIZE 524288

struct TempObject
{
	int     CVertCount;
	float * CVerts;
	int     CNormalCount;
	float * CNormals;
	int     CTexCount;
	float * CTexs;
};


static void CNOVRModelLoadOBJ( cnovr_model * m, const char * filename )
{
	int filelen;
	char * file = FileToString( filename, &filelen );
	char ** splits = SplitStrings( file, "\n", "\r", 1 );
	free( file );

	int flipv = 1;

	struct TempObject t;
	memset( &t, 0, sizeof( t ) );
	if( m->iGeos != 3 )
	{
		CNOVRModelSetNumVBOsWithStrides( m, 3, 3, 4, 3 );
		CNOVRModelSetNumIndices( m, 0 );
		CNOVRModelResetMarks( m );
	}
	CNOVRModelSetNumIndices( m, 0 );

	int indices = 0;
	char * line;
	int lineno;
	int nObjNo = 0;
	for( lineno = 0; line = splits[lineno] ; lineno++ )
	{
		int linelen = strlen( line );
		if( linelen < 2 ) continue;
		if( tolower( line[0] ) == 'v' )
		{
			if( tolower( line[1] ) == 'n' )
			{
				t.CNormals = realloc( t.CNormals, ( t.CNormalCount + 1 ) * 3 * sizeof( float ) );
				int r = sscanf( line + 3, "%f %f %f", 
					&t.CNormals[0 + t.CNormalCount * 3], 
					&t.CNormals[1 + t.CNormalCount * 3], 
					&t.CNormals[2 + t.CNormalCount * 3] );
				t.CNormals[3 + t.CNormalCount * 3] = 0;
				if( r == 3 )
					t.CNormalCount++;
			}
			else if( tolower( line[1] ) == 't' )
			{
				t.CTexs = realloc( t.CTexs, ( t.CTexCount + 1 ) * 4 * sizeof( float ) );
				t.CTexs[1 + t.CTexCount * 4] = 0;
				t.CTexs[2 + t.CTexCount * 4] = 0;
				t.CTexs[3 + t.CTexCount * 4] = nObjNo;
				int r = sscanf( line + 3, "%f %f %f", 
					&t.CTexs[0 + t.CTexCount * 4], 
					&t.CTexs[1 + t.CTexCount * 4], 
					&t.CTexs[2 + t.CTexCount * 4] );

				if( flipv )
					t.CTexs[1 + t.CTexCount * 4] = 1. - t.CTexs[1 + t.CTexCount * 4];
				if( r == 3 || r == 2 )
					t.CTexCount++;
				else
				{
					CNOVRAlert( m, 1, "Error: Invalid Tex Coords (%d) (%s)\n", r, line + 3 );
				}
			}
			else
			{
				t.CVerts = realloc( t.CVerts, ( t.CVertCount + 1 ) * 3 * sizeof( float ) );
				t.CVerts[2 + t.CVertCount * 3] = 0;
				int r = sscanf( line + 2, "%f %f %f", 
					&t.CVerts[0 + t.CVertCount * 3], 
					&t.CVerts[1 + t.CVertCount * 3], 
					&t.CVerts[2 + t.CVertCount * 3] );
				if( r == 3 || r == 2 )
					t.CVertCount++;
				else
				{
					fprintf( stderr, "Error: Invalid Vertex\n" );
				}
			}
		}
		else if( tolower( line[0] ) == 'f' )
		{
			char buffer[3][TBUFFERSIZE];
			int p = 0;
			int r = sscanf( line + 1, "%30s %30s %30s", 
				buffer[0], buffer[1], buffer[2] );

			if( r != 3 )
			{
				//Invalid line - continue.
				continue;
			}

			//Whatever... they're all populated with something now.

			for( p = 0; p < 3; p++ )
			{
				char buffer2[3][TBUFFERSIZE];
				int mark = 0, markb = 0;
				int i;
				int sl = strlen( buffer[p] );
				for( i = 0; i < sl; i++ )
				{
					if( buffer[p][i] == '/' )
					{
						buffer2[mark][markb] = 0;
						mark++;
						if( mark >= 3 ) break;
						markb = 0;
					}
					else
						buffer2[mark][markb++] = buffer[p][i];
				}
				buffer2[mark][markb] = 0;
				for( i = mark+1; i < 3; i++ )
				{
					buffer2[i][0] = '0';
					buffer2[i][1] = 0;
				}

				int VNumber = atoi( buffer2[0] ) - 1;
				int TNumber = atoi( buffer2[1] ) - 1;
				int NNumber = atoi( buffer2[2] ) - 1;

				CNOVRModelTackIndex( m, indices++ );
				CNOVRModelTackIndex( m, indices++ );
				CNOVRModelTackIndex( m, indices++ );

				if( VNumber < t.CVertCount )
					CNOVRVBOTackv( m->pGeos[0], 3, &t.CVerts[VNumber*3] );
				else
					CNOVRVBOTack( m->pGeos[0], 3, 0, 0, 0, 0 );

				if( TNumber < t.CTexCount )

					CNOVRVBOTackv( m->pGeos[1], 4, &t.CTexs[TNumber*4] );
				else
					CNOVRVBOTack( m->pGeos[1], 4, 0, 0, 0, nObjNo );

				if( NNumber < t.CNormalCount )
					CNOVRVBOTackv( m->pGeos[2], 3, &t.CNormals[NNumber*3] );
				else
					CNOVRVBOTack( m->pGeos[2], 3, 0, 0, 0 );
			}
		}
		else if( tolower( line[0] ) == 'o' )
		{
			const char * marker = ( strlen( line ) > 2 )?( line + 2 ):0;
			nObjNo++;
			CNOVRDelinateGeometry( m, marker );
		}
		else if( strncmp( line, "usemtl", 6 ) == 0 )
		{
			//Not implemented.
		}
		else if( strncmp( line, "mtllib", 6 ) == 0 )
		{
			//Not implemented.
		}
		else if( tolower( line[0] ) == 's' )
		{
			//Not implemented.
		}
	}

	if( t.CVerts ) free( t.CVerts ); 
	if( t.CTexs ) free( t.CTexs ); 
	if( t.CNormals ) free( t.CNormals ); 
	free( splits );

	CNOVRVBOTaint( m->pGeos[0] );
	CNOVRVBOTaint( m->pGeos[1] );
	CNOVRVBOTaint( m->pGeos[2] );
	CNOVRModelTaintIndices( m );
	return;
}



static void CNOVRModelLoadRenderModel( cnovr_model * m, char * pchRenderModelName )
{
	RenderModel_t * pModel = NULL;
	while( cnovrstate->oRenderModels->LoadRenderModel_Async( pchRenderModelName, &pModel ) == EVRRenderModelError_VRRenderModelError_Loading)
	{
		OGUSleep( 100 );
	}

	if( cnovrstate->oRenderModels->LoadRenderModel_Async( pchRenderModelName, &pModel ) || pModel == NULL )
	{
		CNOVRAlert( m, 1, "Unable to load render model %s\n", pchRenderModelName );
		return;
	}


	RenderModel_TextureMap_t *pTexture = NULL;

	while( cnovrstate->oRenderModels->LoadTexture_Async( pModel->diffuseTextureId, &pTexture ) == EVRRenderModelError_VRRenderModelError_Loading )
	{
		OGUSleep( 100 );
	}

	if( cnovrstate->oRenderModels->LoadTexture_Async(pModel->diffuseTextureId, &pTexture) || pTexture == NULL )
	{
		CNOVRAlert( m, 1, "Unable to load render model %s\n", pchRenderModelName );
		cnovrstate->oRenderModels->FreeRenderModel( pModel );
		return;
	}

	if( m->iGeos != 3 )
	{
		CNOVRModelSetNumVBOsWithStrides( m, 3, 3, 2, 3 );
		CNOVRModelSetNumIndices( m, 0 );
		CNOVRModelResetMarks( m );
	}
	CNOVRModelSetNumIndices( m, 0 );
	CNOVRDelinateGeometry( m, pchRenderModelName );
	int i;
	for( i = 0; i < pModel->unVertexCount; i++ )
	{
		RenderModel_Vertex_t * v = pModel->rVertexData + i;
		CNOVRVBOTackv( m->pGeos[0], 3, v->vPosition.v );
		CNOVRVBOTackv( m->pGeos[1], 2, v->rfTextureCoord );
		CNOVRVBOTackv( m->pGeos[2], 3, v->vNormal.v );
	}
	for( i = 0; i < pModel->unTriangleCount; i++ )
	{
		CNOVRModelTackIndex( m, 3, pModel->rIndexData[i*3+0], pModel->rIndexData[i*3+1], pModel->rIndexData[i*3+2] );
	}

	if( m->iTextures == 0 )
	{
		m->pTextures = realloc( m->pTextures, sizeof( cnovr_texture * ) * 1 );
		m->pTextures[0] = CNOVRTextureCreate( 1, 1, 4 );
		m->iTextures = 1;
	}
	uint8_t * copyTexture = malloc( pTexture->unWidth * pTexture->unHeight * 4 );
	memcpy( copyTexture, pTexture->rubTextureMapData, pTexture->unWidth * pTexture->unHeight * 4 );
	CNOVRTextureLoadDataAsync( m->pTextures[0], pTexture->unWidth, pTexture->unHeight, 4, 0, copyTexture );

	CNOVRVBOTaint( m->pGeos[0] );
	CNOVRVBOTaint( m->pGeos[1] );
	CNOVRVBOTaint( m->pGeos[2] );

	cnovrstate->oRenderModels->FreeRenderModel( pModel );
	cnovrstate->oRenderModels->FreeTexture( pTexture );
}


void CNOVRModelLoadFromFileAsyncCallback( void * vm, int i )
{
	cnovr_model * m = (cnovr_model*) vm;
	OGLockMutex( m->model_mutex );
	char * filename = m->geofile;
	int slen = strlen( filename );
	if( StringCompareEndingCase( filename, ".obj" ) == 0 )
	{
		CNOVRModelLoadOBJ( m, filename );
	}
	else if( StringCompareEndingCase( filename, ".rendermodel" ) == 0 )
	{
		CNOVRModelLoadRenderModel( m, filename );
	}
	else
	{
		CNOVRAlert( m, 1, "Error: Could not open model file: \"%s\".\n", filename );
	}
	OGUnlockMutex( m->model_mutex );

}

void CNOVRModelLoadFromFileAsync( cnovr_model * m, const char * filename )
{
	OGLockMutex( m->model_mutex );
	if( m->geofile ) free( m->geofile );
	m->geofile = strdup( filename );
	CNOVRJobTack( cnovrQAsync, CNOVRModelLoadFromFileAsyncCallback, m, 0 );
	CNOVRJobTack( cnovrQAsync, CNOVRModelLoadFromFileAsyncCallback, m, 0 );
	OGUnlockMutex( m->model_mutex );
}

