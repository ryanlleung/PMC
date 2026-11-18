/****************************************************************************
**
** Copyright (C) 2024 MikroElektronika d.o.o.
** Contact: https://www.mikroe.com/contact
**
** Commercial License Usage
**
** Licensees holding valid commercial NECTO compilers AI licenses may use this
** file in accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The MikroElektronika Company.
** For licensing terms and conditions see
** https://www.mikroe.com/legal/software-license-agreement.
** For further information use the contact form at
** https://www.mikroe.com/contact.
**
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used for
** non-commercial projects under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
** OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
** DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT
** OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
** OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
****************************************************************************/
/*!
 * @file  core_header.h
 * @brief Mikroe GCC core header implementation.
 */

#ifndef __MIKROC_CORE_LIB_H__
#define __MIKROC_CORE_LIB_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "delay_parameters.h"

#define ADDRESS_RCC_CR 0x40023800 
#define VALUE_RCC_CR 0x1000081 
#define ADDRESS_RCC_PLLCFGR 0x40023804 
#define VALUE_RCC_PLLCFGR 0x9005410 
#define ADDRESS_RCC_CFGR 0x40023808 
#define VALUE_RCC_CFGR 0x609402 
#define ADDRESS_SVRANGE 0x400FE070 
#define VALUE_SVRANGE 0x3 
#define FOSC_KHZ_VALUE 168000 
#define FOSC_KHZ_VALUE_DEFINED 
#define STM32F407ZG 
#define MCU_NAME_DEFINED

#ifdef __cplusplus
}
#endif

#endif // __MIKROC_CORE_LIB_H__
