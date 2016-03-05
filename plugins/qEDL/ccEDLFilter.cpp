//##########################################################################
//#                                                                        #
//#                       CLOUDCOMPARE PLUGIN: qEDL                        #
//#                                                                        #
//#  This program is free software; you can redistribute it and/or modify  #
//#  it under the terms of the GNU General Public License as published by  #
//#  the Free Software Foundation; version 2 of the License.               #
//#                                                                        #
//#  This program is distributed in the hope that it will be useful,       #
//#  but WITHOUT ANY WARRANTY; without even the implied warranty of        #
//#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         #
//#  GNU General Public License for more details.                          #
//#                                                                        #
//#          COPYRIGHT: EDF R&D / TELECOM ParisTech (ENST-TSI)             #
//#                                                                        #
//##########################################################################

#include "ccEDLFilter.h"

//ccFBO
#include <ccFrameBufferObject.h>
#include <ccBilateralFilter.h>
#include <ccShader.h>
//qCC_gl
#include <ccGLUtils.h>

//Qt
#include <QOpenGLContext>

//system
#include <math.h>
#include <assert.h>

//For MSVC
#ifndef M_PI
#define M_PI 3.141592653589793238462643
#endif

ccEDLFilter::ccEDLFilter()
	: ccGlFilter("EyeDome Lighting (disable normals and increase points size for a better result!)")
	, m_screenWidth(0)
	, m_screenHeight(0)
	, fbo_edl0(0)
	, fbo_edl1(0)
	, fbo_edl2(0)
	, shader_edl(0)
	, fbo_mix(0)
	, shader_mix(0)
	, exp_scale(100.0f)
{
	//smoothing filter for full resolution
	m_bilateralFilter0.enabled	= false;
	m_bilateralFilter0.halfSize	= 1;
	m_bilateralFilter0.sigma	= 1.0f;
	m_bilateralFilter0.sigmaZ	= 0.2f;

	//smoothing filter for half resolution
	m_bilateralFilter1.enabled	= true;
	m_bilateralFilter1.halfSize	= 2;
	m_bilateralFilter1.sigma	= 2.0f;
	m_bilateralFilter1.sigmaZ	= 0.4f;

	//smoothing filter for quarter resolution
	m_bilateralFilter2.enabled	= true;
	m_bilateralFilter2.halfSize	= 2;
	m_bilateralFilter2.sigma	= 2.0f;
	m_bilateralFilter2.sigmaZ	= 0.4f;

	setLightDir(static_cast<float>(M_PI*0.5), static_cast<float>(M_PI*0.5));

	memset(neighbours, 0, sizeof(float)*8*2);
	for (int c = 0; c<8; c++)
	{
		neighbours[2 * c]     = static_cast<float>(cos(static_cast<double>(c)*M_PI / 4));
		neighbours[2 * c + 1] = static_cast<float>(sin(static_cast<double>(c)*M_PI / 4));
	}
}

ccEDLFilter::~ccEDLFilter()
{
	reset();
}

ccGlFilter* ccEDLFilter::clone() const
{
	ccEDLFilter* filter = new ccEDLFilter();

	//copy parameters (only those that can be changed by the user!)
	filter->setStrength(exp_scale);
	filter->light_dir[0] = light_dir[0];
	filter->light_dir[1] = light_dir[1];
	filter->light_dir[2] = light_dir[2];

	return filter;
}

void ccEDLFilter::reset()
{
	if (fbo_edl0)
		delete fbo_edl0;
	fbo_edl0 = 0;

	if (fbo_edl1)
		delete fbo_edl1;
	fbo_edl1 = 0;

	if (fbo_edl2)
		delete fbo_edl2;
	fbo_edl2 = 0;

	if (fbo_mix)
		delete fbo_mix;
	fbo_mix = 0;

	if (shader_edl)
		delete shader_edl;
	shader_edl = 0;

	if (shader_mix)
		delete shader_mix;
	shader_mix = 0;

	if (m_bilateralFilter0.filter)
		delete m_bilateralFilter0.filter;
	m_bilateralFilter0.filter = 0;

	if (m_bilateralFilter1.filter)
		delete m_bilateralFilter1.filter;
	m_bilateralFilter1.filter = 0;

	if (m_bilateralFilter2.filter)
		delete m_bilateralFilter2.filter;
	m_bilateralFilter2.filter = 0;

	m_screenWidth = m_screenHeight = 0;
}

bool ccEDLFilter::init(QOpenGLContext* context, int width, int height, QString shadersPath, QString& error)
{
	return init(context, width, height, GL_RGBA, GL_LINEAR, shadersPath, error);
}

bool ccEDLFilter::init(QOpenGLContext* context, int width, int height, GLenum internalFormat, GLenum minMagFilter, QString shadersPath, QString& error)
{
	if (!context)
	{
		assert(false);
		return false;
	}

	QOpenGLFunctions_3_0* gl30Func = context->versionFunctions<QOpenGLFunctions_3_0>();
	if (!gl30Func)
	{
		error = ccLog::Error("[EDL] Initialization failed! (OpenGL 3.0 not supported)");
		return false;
	}

	if (!fbo_edl0)
	{
		fbo_edl0 = new ccFrameBufferObject();
	}
	if (!fbo_edl0->init(width, height, gl30Func))
	{
		error = "[EDL Filter] FBO 1:1 initialization failed!";
		reset();
		return false;
	}

	if (!fbo_edl1)
	{
		fbo_edl1 = new ccFrameBufferObject();
	}
	if (!fbo_edl1->init(width / 2, height / 2, gl30Func))
	{
		error = "[EDL Filter] FBO 1:2 initialization failed!";
		reset();
		return false;
	}

	if (!fbo_edl2)
	{
		fbo_edl2 = new ccFrameBufferObject();
	}
	if (!fbo_edl2->init(width / 4, height / 4, gl30Func))
	{
		error = "[EDL Filter] FBO 1:4 initialization failed!";
		reset();
		return false;
	}

	fbo_edl0->initColor(internalFormat, GL_RGBA, GL_FLOAT, minMagFilter);
	fbo_edl1->initColor(internalFormat, GL_RGBA, GL_FLOAT, minMagFilter);
	fbo_edl2->initColor(internalFormat, GL_RGBA, GL_FLOAT, minMagFilter);

	if (!fbo_mix)
	{
		fbo_mix = new ccFrameBufferObject();
	}
	if (!fbo_mix->init(width, height, gl30Func))
	{
		error = "[EDL Filter] FBO 'mix' initialization failed!";
		reset();
		return false;
	}
	fbo_mix->initColor(internalFormat, GL_RGBA, GL_FLOAT);

	if (!shader_edl)
	{
		shader_edl = new ccShader();
		if (!shader_edl->fromFile(shadersPath,"EDL/edl_shade",error))
		{
			reset();
			return false;
		}
	}

	if (!shader_mix)
	{
		shader_mix = new ccShader();
		if (!shader_mix->fromFile(shadersPath,"EDL/edl_mix",error))
		{
			reset();
			return false;
		}
	}

	if (m_bilateralFilter0.enabled)
	{
		if (!m_bilateralFilter0.filter)
		{
			m_bilateralFilter0.filter = new ccBilateralFilter();
		}
		if (!m_bilateralFilter0.filter->init(context, width, height, shadersPath, error))
		{
			delete m_bilateralFilter0.filter;
			m_bilateralFilter0.filter = 0;
			m_bilateralFilter0.enabled = false;
		}
		else
		{
			m_bilateralFilter0.filter->useExistingViewport(true);
		}
	}
	else if (m_bilateralFilter0.filter)
	{
		delete m_bilateralFilter0.filter;
		m_bilateralFilter0.filter = 0;
	}

	if (m_bilateralFilter1.enabled)
	{
		if (!m_bilateralFilter1.filter)
		{
			m_bilateralFilter1.filter = new ccBilateralFilter();
		}
		if (!m_bilateralFilter1.filter->init(context, width / 2, height / 2, shadersPath, error))
		{
			delete m_bilateralFilter1.filter;
			m_bilateralFilter1.filter = 0;
			m_bilateralFilter1.enabled = false;
		}
		else
		{
			m_bilateralFilter1.filter->useExistingViewport(true);
		}
	}
	else if (m_bilateralFilter1.filter)
	{
		delete m_bilateralFilter1.filter;
		m_bilateralFilter1.filter = 0;
	}

	if (m_bilateralFilter2.enabled)
	{
		if (!m_bilateralFilter2.filter)
		{
			m_bilateralFilter2.filter = new ccBilateralFilter();
		}
		if (!m_bilateralFilter2.filter->init(context, width / 4, height / 4, shadersPath, error))
		{
			delete m_bilateralFilter2.filter;
			m_bilateralFilter2.filter = 0;
			m_bilateralFilter2.enabled = false;
		}
		else
		{
			m_bilateralFilter2.filter->useExistingViewport(true);
		}
	}
	else if (m_bilateralFilter2.filter)
	{
		delete m_bilateralFilter2.filter;
		m_bilateralFilter2.filter = 0;
	}

	m_screenWidth = width;
	m_screenHeight = height;

	return true;
}

void ccEDLFilter::shade(QOpenGLContext* context, GLuint texDepth, GLuint texColor, ViewportParameters& parameters)
{
	if (!context)
	{
		assert(false);
		return;
	}

	QOpenGLFunctions_2_1* glFunc = context->versionFunctions<QOpenGLFunctions_2_1>();
	if (!glFunc)
	{
		return;
	}

	if (!fbo_edl0)
	{
		//ccLog::Warning("[ccEDLFilter::shade] Internal error: structures not initialized!");
		return;
	}

	if (m_screenWidth < 4 || m_screenHeight < 4)
	{
		//ccLog::Warning("[ccEDLFilter::shade] Screen is too small!");
		return;
	}

	//perspective mode
	int perspectiveMode = parameters.perspectiveMode ? 1 : 0;
	//light-balancing based on the current zoom (for ortho. mode only)
	float lightMod = perspectiveMode ? 3.0f : static_cast<float>(sqrt(2.0*std::max<double>(parameters.zoom,0.7))); //1.41 ~ sqrt(2)

	glFunc->glPushAttrib(GL_ALL_ATTRIB_BITS);

	//we must use corner-based screen coordinates
	glFunc->glMatrixMode(GL_PROJECTION);
	glFunc->glPushMatrix();
	glFunc->glLoadIdentity();
	glFunc->glOrtho(0.0, (GLdouble)m_screenWidth, 0.0, (GLdouble)m_screenHeight, 0.0, 1.0/*parameters.zNear,parameters.zFar*/);
	glFunc->glMatrixMode(GL_MODELVIEW);
	glFunc->glPushMatrix();
	glFunc->glLoadIdentity();

	/***	FULL SIZE	***/
	{
		fbo_edl0->start();

		shader_edl->bind();
		shader_edl->setUniformValue("s1_color", 1);
		shader_edl->setUniformValue("s2_depth", 0);

		shader_edl->setUniformValue("Sx", static_cast<float>(m_screenWidth));
		shader_edl->setUniformValue("Sy", static_cast<float>(m_screenHeight));
		shader_edl->setUniformValue("Zoom", lightMod);
		shader_edl->setUniformValue("PerspectiveMode", perspectiveMode);
		shader_edl->setUniformValue("Pix_scale", 1.0f);
		shader_edl->setUniformValue("Exp_scale", exp_scale);
		shader_edl->setUniformValue("Zm", static_cast<float>(parameters.zNear));
		shader_edl->setUniformValue("ZM", static_cast<float>(parameters.zFar));
		shader_edl->setUniformValueArray("Light_dir", reinterpret_cast<const GLfloat*>(light_dir), 1, 3);
		shader_edl->setUniformValueArray("Neigh_pos_2D", reinterpret_cast<const GLfloat*>(neighbours), 8, 2);

		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glEnable(GL_TEXTURE_2D);
		glFunc->glBindTexture(GL_TEXTURE_2D, texColor);

		glFunc->glActiveTexture(GL_TEXTURE0);
		ccGLUtils::DisplayTexture2DPosition(context, texDepth, 0, 0, m_screenWidth, m_screenHeight);

		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glBindTexture(GL_TEXTURE_2D, 0);
		glFunc->glDisable(GL_TEXTURE_2D);

		shader_edl->release();
		fbo_edl0->stop();
	}
	/***	FULL SIZE	***/

	/***	HALF SIZE	***/
	{
		fbo_edl1->start();

		shader_edl->bind();
		shader_edl->setUniformValue("s1_color",1);
		shader_edl->setUniformValue("s2_depth",0);

		shader_edl->setUniformValue("Sx",static_cast<float>(m_screenWidth>>1));
		shader_edl->setUniformValue("Sy",static_cast<float>(m_screenHeight>>1));
		shader_edl->setUniformValue("Zoom",lightMod);
		shader_edl->setUniformValue("PerspectiveMode",perspectiveMode);
		shader_edl->setUniformValue("Pix_scale",2.0f);
		shader_edl->setUniformValue("Exp_scale",exp_scale);
		shader_edl->setUniformValue("Zm",static_cast<float>(parameters.zNear));
		shader_edl->setUniformValue("ZM",static_cast<float>(parameters.zFar));
		shader_edl->setUniformValueArray("Light_dir", reinterpret_cast<const GLfloat*>(light_dir), 1, 3);
		shader_edl->setUniformValueArray("Neigh_pos_2D", reinterpret_cast<const GLfloat*>(neighbours), 8, 2);
		//*/

		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glEnable(GL_TEXTURE_2D);
		glFunc->glBindTexture(GL_TEXTURE_2D, texColor);

		glFunc->glActiveTexture(GL_TEXTURE0);
		ccGLUtils::DisplayTexture2DPosition(context, texDepth, 0, 0, m_screenWidth / 2, m_screenHeight / 2);

		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glBindTexture(GL_TEXTURE_2D, 0);
		glFunc->glDisable(GL_TEXTURE_2D);

		shader_edl->release();
		fbo_edl1->stop();
	}
	/***	HALF SIZE	***/

	/***	QUARTER SIZE	***/
	{
		fbo_edl2->start();

		shader_edl->bind();
		shader_edl->setUniformValue("s1_color",1);
		shader_edl->setUniformValue("s2_depth",0);

		shader_edl->setUniformValue("Sx",static_cast<float>(m_screenWidth>>2));
		shader_edl->setUniformValue("Sy",static_cast<float>(m_screenHeight>>2));
		shader_edl->setUniformValue("Zoom",lightMod);
		shader_edl->setUniformValue("PerspectiveMode",perspectiveMode);
		shader_edl->setUniformValue("Pix_scale",4.0f);
		shader_edl->setUniformValue("Exp_scale",exp_scale);
		shader_edl->setUniformValue("Zm",static_cast<float>(parameters.zNear));
		shader_edl->setUniformValue("ZM",static_cast<float>(parameters.zFar));
		shader_edl->setUniformValueArray("Light_dir", reinterpret_cast<const GLfloat*>(light_dir), 1, 3);
		shader_edl->setUniformValueArray("Neigh_pos_2D", reinterpret_cast<const GLfloat*>(neighbours), 8, 2);
		//*/

		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glEnable(GL_TEXTURE_2D);
		glFunc->glBindTexture(GL_TEXTURE_2D, texColor);

		glFunc->glActiveTexture(GL_TEXTURE0);
		ccGLUtils::DisplayTexture2DPosition(context, texDepth, 0, 0, m_screenWidth / 4, m_screenHeight / 4);

		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glBindTexture(GL_TEXTURE_2D, 0);
		glFunc->glDisable(GL_TEXTURE_2D);

		shader_edl->release();
		fbo_edl2->stop();
	}
	/***	QUARTER SIZE	***/

	/***	SMOOTH RESULTS	***/
	{
		if (m_bilateralFilter0.filter)
		{
			m_bilateralFilter0.filter->setParams(m_bilateralFilter0.halfSize,m_bilateralFilter0.sigma,m_bilateralFilter0.sigmaZ);
			m_bilateralFilter0.filter->shade(context, texDepth, fbo_edl0->getColorTexture(), parameters);
		}
		if (m_bilateralFilter1.filter)
		{
			m_bilateralFilter1.filter->setParams(m_bilateralFilter1.halfSize,m_bilateralFilter1.sigma,m_bilateralFilter1.sigmaZ);
			m_bilateralFilter1.filter->shade(context, texDepth, fbo_edl1->getColorTexture(), parameters);
		}
		if (m_bilateralFilter2.filter)
		{
			m_bilateralFilter2.filter->setParams(m_bilateralFilter2.halfSize,m_bilateralFilter2.sigma,m_bilateralFilter2.sigmaZ);
			m_bilateralFilter2.filter->shade(context, texDepth, fbo_edl2->getColorTexture(), parameters);
		}
	}
	/***	SMOOTH RESULTS	***/

	//***	COMPOSITING		***/
	{
		fbo_mix->start();

		shader_mix->bind();
		shader_mix->setUniformValue("s2_I1",0);
		shader_mix->setUniformValue("s2_I2",1);
		shader_mix->setUniformValue("s2_I4",2);
		shader_mix->setUniformValue("s2_D",3);
		shader_mix->setUniformValue("A0",1.0f);
		shader_mix->setUniformValue("A1",0.5f);
		shader_mix->setUniformValue("A2",0.25f);
		shader_mix->setUniformValue("absorb",1);

		glFunc->glActiveTexture(GL_TEXTURE3);
		glFunc->glEnable(GL_TEXTURE_2D);
		glFunc->glBindTexture(GL_TEXTURE_2D, texDepth);

		glFunc->glActiveTexture(GL_TEXTURE2);
		glFunc->glEnable(GL_TEXTURE_2D);
		glFunc->glBindTexture(GL_TEXTURE_2D, m_bilateralFilter2.filter ? m_bilateralFilter2.filter->getTexture() : fbo_edl2->getColorTexture());

		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glEnable(GL_TEXTURE_2D);
		glFunc->glBindTexture(GL_TEXTURE_2D, m_bilateralFilter1.filter ? m_bilateralFilter1.filter->getTexture() : fbo_edl1->getColorTexture());

		glFunc->glActiveTexture(GL_TEXTURE0);
		//glFunc->glEnable(GL_TEXTURE_2D);

		ccGLUtils::DisplayTexture2DPosition(context,
											m_bilateralFilter0.filter ? m_bilateralFilter0.filter->getTexture() : fbo_edl0->getColorTexture(),
											0 , 0,
											m_screenWidth, m_screenHeight);

		//glFunc->glActiveTexture(GL_TEXTURE0);
		//glFunc->glBindTexture(GL_TEXTURE_2D,0);
		//glFunc->glDisable(GL_TEXTURE_2D);
		glFunc->glActiveTexture(GL_TEXTURE1);
		glFunc->glBindTexture(GL_TEXTURE_2D,0);
		glFunc->glDisable(GL_TEXTURE_2D);
		glFunc->glActiveTexture(GL_TEXTURE2);
		glFunc->glBindTexture(GL_TEXTURE_2D,0);
		glFunc->glDisable(GL_TEXTURE_2D);
		glFunc->glActiveTexture(GL_TEXTURE3);
		glFunc->glBindTexture(GL_TEXTURE_2D,0);
		glFunc->glDisable(GL_TEXTURE_2D);

		shader_mix->release();
		fbo_mix->stop();
	}

	glFunc->glMatrixMode(GL_PROJECTION);
	glFunc->glPopMatrix();
	glFunc->glMatrixMode(GL_MODELVIEW);
	glFunc->glPopMatrix();

	glFunc->glPopAttrib();
}

GLuint ccEDLFilter::getTexture()
{
	return getTexture(0);
}

GLuint ccEDLFilter::getTexture(int index)
{
	switch (index)
	{
	case 0:
		return (fbo_mix ? fbo_mix->getColorTexture() : 0);
	case 1:
		return (fbo_edl0 ? fbo_edl0->getColorTexture() : 0);
	case 2:
		return (fbo_edl1 ? fbo_edl1->getColorTexture() : 0);
	case 3:
		return (fbo_edl2 ? fbo_edl2->getColorTexture() : 0);
	case 4:
		return (m_bilateralFilter0.filter ? m_bilateralFilter0.filter->getTexture() : 0);
	case 5:
		return (m_bilateralFilter1.filter ? m_bilateralFilter1.filter->getTexture() : 0);
	case 6:
		return (m_bilateralFilter2.filter ? m_bilateralFilter2.filter->getTexture() : 0);
	}

	//ccLog::Warning("[ccEDLFilter::getTexture] Internal error: bad argument");
	return 0;
}

void ccEDLFilter::setLightDir(float theta_rad, float phi_rad)
{
	light_dir[0]	=	sin(phi_rad)*cos(theta_rad);
	light_dir[1]	=	cos(phi_rad);
	light_dir[2]	=	sin(phi_rad)*sin(theta_rad);
}
